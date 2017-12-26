#include <iostream>
#include <fstream>
#include <string>
#include <sstream>
#include <vector>
#include <memory>
#include <iomanip>
#include <chrono>
#include <cmath>
#include <sksat/math/vector.hpp>
#include "common.hpp"

#define OMP_CHUNK_NUM	64
#define CHECK_DT
//#define CHECK_SAME_POS

namespace simulation {
	size_t time_step = 0;
	Float time=.0;
	Float dt=0.0001, finish_time=60.0;
	size_t dim=3;
	const size_t progress_interval = 100;
	size_t output_interval = 500;

	// 定数たち
	Float pcl_dst	= 0.02;		// 平均粒子間距離(今は決め打ち)
	const Float crt_num	= 0.1;		// クーラン条件数
	const Float knm_vsc	= 0.000001;	// 動粘性係数
	const Float dens_fluid	= 1000;		// 液体粒子の密度
	const Float dens_wall	= 1000;		// 壁粒子の密度
	const Float dst_lmt_rat	= 0.9;		// これ以上の粒子間の接近を許さない距離の係数
	const Float col_rat	= 0.2;		// 接近した粒子の反発率
	const Float col		= 1.0 + col_rat;
	const Float sound_vel	= 22.0;		// 流体の音速(?)

	size_t particle_number=0;
	std::vector< sksat::math::vector<Float> > acc, vel, pos;
	std::vector<Float> press;
	std::vector<int> type;
	sksat::math::vector<Float> min, max;

	Float r, r2;			// 影響半径
	Float A1;			// 粘性項の計算に用いる係数
	Float A2;			// 圧力の計算に用いる係数
	Float A3;			// 圧力勾配項の計算に用いる係数
	Float n0;			// 初期粒子数密度
	Float lmd;			// ラプラシアンモデルの係数λ
	Float dens[NUM_TYPE];		// 粒子種類ごとの密度
	Float dens_inv[NUM_TYPE];
	Float rlim, rlim2;		// これ以上の粒子間の接近を許さない距離

	namespace bucket {
		Float width, width2, width_inv;
		size_t nx, ny, nz;
		size_t nxy, nxyz;
		std::unique_ptr<int[]> first, last, next;
	}

	size_t file_number = 0;

	template<typename T>
	inline T weight(T dist, T re){ return ((re/dist) - 1.0); } // 重み関数

	void main_loop();
	void make_bucket();
	void calc_tmpacc();
	bool check_overflow(size_t i);
	void move_particle_tmp();
	void check_collision();
	void make_press();
	void calc_press_grad(); // 圧力勾配項
	void move_particle();
	void move_body();

	void load_file(const std::string &fname);
	void alloc_bucket();
	void set_param();
	void write_file(const size_t &step, const Float &time);
}

int usage(const char *s){
	std::cout << "> "<< s << " input_file" << std::endl;
	return -1;
}

int main(int argc, char **argv){
	if(argc != 2)
		return usage(argv[0]);

	simulation::load_file(argv[1]);
	simulation::alloc_bucket();
	simulation::set_param();

	std::cout<<" *** START SIMULATION *** "<<std::endl;

	auto start = std::chrono::system_clock::now();
	simulation::main_loop();
	auto end = std::chrono::system_clock::now();

	double elapsed = std::chrono::duration_cast<std::chrono::seconds>(end-start).count(); ;
	std::cout<<"Total : "<<elapsed<<"sec"<<std::endl;

	std::cout<<" ***  END SIMULATION  *** "<<std::endl;
	return 0;
}

#define PRINT(v) {std::cout<< #v <<": "<<v<<std::endl;}

void simulation::load_file(const std::string &fname){
	std::cout<<"loading file \""<<fname<<"\"..."<<std::endl;
	std::ifstream f(fname);
	f >> time_step
		>> time
		>> dim
		>> rw::r_in >> rw::r_out
		>> rw::theta >> rw::w
		>> particle_number;

	PRINT(time_step);
	PRINT(time);
	PRINT(dim);
	PRINT(rw::r_in);
	PRINT(rw::r_out);
	PRINT(rw::theta);
	PRINT(rw::w);
	PRINT(particle_number);

	pcl_dst = ((rw::r_out-rw::r_in)+rw::r_out)/static_cast<Float>(NUM_PER_DST);
	PRINT(pcl_dst);

	acc.reserve(particle_number);
	vel.reserve(particle_number);
	pos.reserve(particle_number);
	press.reserve(particle_number);
	type.reserve(particle_number);

	Float pav;
	for(auto i=0;i<particle_number;i++){
		f >> type[i]
			>> pos[i].x
			>> pos[i].y
			>> pos[i].z
			>> vel[i].x
			>> vel[i].y
			>> vel[i].z
			>> press[i]
			>> pav;
		if(f.eof()){
			std::cerr<<"stop: "<<i<<std::endl;
			throw std::runtime_error("");
		}
	}

	auto tmp = rw::r_out - rw::r_in;
	min.x = min.y = (rw::r_out + tmp) * -1.0 - pcl_dst;
	min.z = -1.0 * pcl_dst;
	max.x = max.y = (rw::r_out + tmp) + pcl_dst;
	max.z = pcl_dst;

	PRINT(min.x);
	PRINT(min.y);
	PRINT(min.z);
	PRINT(max.x);
	PRINT(max.y);
	PRINT(max.z);

	for(auto i=0;i<particle_number;i++){
/*		if(
			pos[i].x < min.x || max.x < pos[i].x ||
			pos[i].y < min.y || max.y < pos[i].y ||
			pos[i].z < min.z || max.z < pos[i].z
		){
			std::cout<<"error: "<<std::endl;
			PRINT(pos[i].x);
			PRINT(pos[i].y);
			PRINT(pos[i].z);
			getchar();
		}
*/
		check_overflow(i);
	}
}

void simulation::alloc_bucket(){
	using namespace bucket;

	r = pcl_dst * 2.1;
	r2 = r*r;

	width = r * (1.0 + crt_num); // バケット一辺の長さ
	width2 = width * width;
	width_inv = 1.0 / width;

	nx = static_cast<int>((max.x - min.x) * width_inv) + 3;
	ny = static_cast<int>((max.y - min.y) * width_inv) + 3;
	nz = static_cast<int>((max.z - min.z) * width_inv) + 3;
	nxy  = nx * ny;
	nxyz = nx * ny * nz;

	PRINT(bucket::nx);
	PRINT(bucket::ny);
	PRINT(bucket::nxy);
	PRINT(bucket::nxyz);

	bucket::first = std::make_unique<int[]>(nxyz);
	bucket::last = std::make_unique<int[]>(nxyz);
	bucket::next  = std::make_unique<int[]>(particle_number);
}

void simulation::set_param(){
	n0 = lmd = 0.0;
	for(int ix=-4;ix<5;ix++){
		for(int iy=-4;iy<5;iy++){
			for(int iz=-4;iz<5;iz++){
				Float x = pcl_dst * static_cast<double>(ix);
				Float y = pcl_dst * static_cast<double>(iy);
				Float z = pcl_dst * static_cast<double>(iz);
				Float dist2 = x*x + y*y + z*z;
				if(dist2 <= r2){
					if(dist2 == 0.0) continue;
					Float dist = sqrt(dist2);
					n0  += weight(dist, r);
					lmd += dist2 * weight(dist, r);
				}
			}
		}
	}
	lmd	= lmd / n0;
	A1 = 2.0 * knm_vsc * static_cast<Float>(dim) / (n0 * lmd);
	A2 = sound_vel * sound_vel / n0;
	A3 = -1.0 * static_cast<Float>(dim) / n0;

	PRINT(n0);
	PRINT(lmd);
	PRINT(A1);
	PRINT(A2);
	PRINT(A3);

	dens[FLUID]	= dens_fluid;
	dens[WALL]	= dens_wall;
	dens_inv[FLUID]	= 1.0/dens_fluid;
	dens_inv[WALL]	= 1.0/dens_wall;

	rlim	= pcl_dst * dst_lmt_rat;
	rlim2	= rlim * rlim;

	PRINT(dens[FLUID]);
	PRINT(dens[WALL]);
	PRINT(rlim);

	auto D = 0.9;
	if(
		dt < (D * (pcl_dst * pcl_dst) / knm_vsc) &&
		dt < (pcl_dst / sound_vel)
	){
		std::cout<<"dt is ok!"<<std::endl;
	}else{
		throw std::runtime_error("dt is not ok.");
	}
}

void simulation::main_loop(){
	if(time_step == 0)
		write_file(0, time);
	while(true){
		make_bucket();

		calc_tmpacc();
		Float max_vel = 0.0;
#ifdef CHECK_DT
		for(auto i=0;i<particle_number;i++){
			if(type[i] != FLUID) continue;
			auto v = vel[i].x*vel[i].x + vel[i].y*vel[i].y;
			if(max_vel < v) max_vel = v;
#ifdef CHECK_SAME_POS
			for(auto j=0;j<particle_number;j++){
				if(i == j) continue;
				if(type[i] != FLUID) continue;
				if(pos[i].x == pos[j].x && pos[i].y == pos[j].y && pos[i].z == pos[j].z){
					PRINT(i);
					PRINT(j);
					PRINT(pos[i].x);
					PRINT(pos[j].x);
					PRINT(pos[i].y);
					PRINT(pos[j].y);
					throw std::runtime_error("fuck NVIDIA!");
				}
			}
#endif
		}
#endif
		if(dt < (0.2 * pcl_dst / max_vel)){}
		else{
			std::cout<<"max-vel="<<max_vel<<std::endl;
//			throw std::runtime_error("dt is not ok.");
//			dt = dt/2;
//			output_interval*=2;
		}
		move_particle_tmp(); // temporary
		check_collision();
		make_press();
		calc_press_grad();
		move_particle();
		make_press();

		move_body();

		time_step++;
		time += dt;
		if( (time_step % progress_interval) == 0 ){
			std::cout
				<< std::setw(2) << static_cast<int>((time/finish_time)*100) << "% "
				<< "time step: " << std::setw(10) << time_step << "  "
				<< "time: " << std::setw(10) << time << "  "
				<< "particle number: " << particle_number
				<< std::endl;
		}

		if( (time_step % output_interval) == 0){
			write_file(time_step, time);
		}
		if(time >= finish_time) break;
	}
}

void simulation::make_bucket(){
	using namespace bucket;
	for(int i=0;i<nxyz;i++){ first[i] = -1; last[i] = -1; }
	for(int i=0;i<particle_number;i++){ next[i] = -1; }
	for(int i=0;i<particle_number;i++){
		if(type[i] == GHOST) continue;
		int ix = static_cast<int>((pos[i].x - min.x) * width_inv) + 1;
		int iy = static_cast<int>((pos[i].y - min.y) * width_inv) + 1;
		int iz = static_cast<int>((pos[i].z - min.z) * width_inv) + 1;
		int ib = iz*static_cast<int>(nxy) + iy*static_cast<int>(nx) + ix;
		int j = last[ib];
		last[ib] = i;
		if(j == -1){ first[ib] = i; }
		else{ next[j] = i; }
	}
}

void simulation::calc_tmpacc(){
#pragma omp parallel for schedule(dynamic,OMP_CHUNK_NUM)
	for(auto i=0;i<particle_number;i++){
		if(type[i] != FLUID) continue;
		sksat::math::vector<Float> Acc;
		Acc.x = Acc.y = Acc.z = 0.0;
		int ix = static_cast<int>((pos[i].x - min.x) * bucket::width_inv) + 1;
		int iy = static_cast<int>((pos[i].y - min.y) * bucket::width_inv) + 1;
		int iz = static_cast<int>((pos[i].z - min.z) * bucket::width_inv) + 1;
		for(int jz=iz-1;jz<=iz+1;jz++){
			for(int jy=iy-1;jy<=iy+1;jy++){
				for(int jx=ix-1;jx<=ix+1;jx++){
					int jb = jz*static_cast<int>(bucket::nxy) + jy*static_cast<int>(bucket::nx) + jx;
					if(jb >= bucket::nxyz){
						PRINT(i);
						PRINT(type[i]);
						PRINT(pos[i].x);
						PRINT(pos[i].y);
						PRINT(pos[i].z);
						PRINT(ix);
						PRINT(iy);
						PRINT(iz);
						PRINT(jx);
						PRINT(jy);
						PRINT(jz);
						PRINT(jb);
						getchar();
					}
					int j = bucket::first[jb];
					if(j == -1) continue;
					for(;;){
						Float v0 = pos[j].x - pos[i].x;
						Float v1 = pos[j].y - pos[i].y;
						Float v2 = pos[j].z - pos[i].z;
						Float dist2 = v0*v0 + v1*v1 + v2*v2;
						if(dist2 < r2){
							if(j!=i && type[j]!=GHOST){
								Float dist = sqrt(dist2);
								Float w = weight(dist, r);
								Acc += (vel[j] - vel[i]) * w;
							}
						}
						j = bucket::next[j];
						if(j == -1) break;
					}
				}
			}
		}
		acc[i] = Acc * A1;
//		acc[i].y += -9.8;
		if(pos[i].x > 0.0 && -0.05 < pos[i].y && pos[i].y < 0.05) acc[i].y += 0.1;
	}
}

bool simulation::check_overflow(size_t i){
	if(
		pos[i].x > max.x || pos[i].x < min.x ||
		pos[i].y > max.y || pos[i].y < min.y ||
		pos[i].z > max.z || pos[i].z < min.z
	  ){
		std::cout<<"over"<<std::endl;
		type[i] = GHOST;
		press[i] = vel[i].x = vel[i].y = 0.0;
		return true;
	}
	return false;
}

void simulation::move_particle_tmp(){
#pragma omp parallel for
	for(auto i=0;i<particle_number;i++){
		if(type[i] != FLUID) continue;
		vel[i] += acc[i] * dt;
		pos[i] += vel[i] * dt;
		acc[i].x = acc[i].y = acc[i].z = 0.0;
		check_overflow(i);
//		if(check_overflow(i)) std::cout<<"move_particle_tmp: "<<i<<std::endl;
	}
}

void simulation::check_collision(){
#pragma omp parallel for schedule(dynamic,OMP_CHUNK_NUM)
	for(auto i=0;i<particle_number;i++){
		if(type[i] != FLUID) continue;
		Float mi = dens[type[i]];
		auto vec_i = vel[i], vec_i2 = vel[i];
		int ix = static_cast<int>((pos[i].x - min.x) * bucket::width_inv) + 1;
		int iy = static_cast<int>((pos[i].y - min.y) * bucket::width_inv) + 1;
		int iz = static_cast<int>((pos[i].z - min.z) * bucket::width_inv) + 1;
		for(int jz=iz-1;jz<=iz+1;jz++){
			for(int jy=iy-1; jy<=iy+1; jy++){
				for(int jx=ix-1; jx<=ix+1; jx++){
					int jb = jz*bucket::nxy + jy*bucket::nx + jx;
					int j = bucket::first[jb];
					if(j == -1) continue;
					for(;;){
						Float v0 = pos[j].x - pos[i].x;
						Float v1 = pos[j].y - pos[i].y;
						Float v2 = pos[j].z - pos[i].z;
						Float dist2 = v0*v0 + v1*v1 + v2*v2;
						if(dist2 < rlim2){
							if(j!=i && type[i]!= GHOST){
								Float fDT = (vec_i.x-vel[j].x)*v0 + (vec_i.y-vel[j].y)*v1 + (vec_i.z-vel[j].z)*v2;
								if(fDT > 0.0){
									Float mj = dens[type[j]];
									fDT *= col * mj / (mi+mj) / dist2;
									vec_i2.x -= v0 * fDT;
									vec_i2.y -= v1 * fDT;
									vec_i2.z -= v2 * fDT;
								}
							}
						}
						j = bucket::next[j]; // バケット内の次の粒子へ
						if(j == -1) break;
					}
				}
			}
		}
		acc[i] = vec_i2;
	}
	for(auto i=0;i<particle_number;i++){
		vel[i] = acc[i];
	}
}

void simulation::make_press(){
#pragma omp parallel for schedule(dynamic,OMP_CHUNK_NUM)
	for(auto i=0;i<particle_number;i++){
		if(type[i] == GHOST) continue;
		Float ni = 0.0;
		int ix = static_cast<int>((pos[i].x - min.x)*bucket::width_inv) + 1;
		int iy = static_cast<int>((pos[i].y - min.y)*bucket::width_inv) + 1;
		int iz = static_cast<int>((pos[i].z - min.z)*bucket::width_inv) + 1;
		for(int jz=iz-1;jz<=iz+1;jz++){
			for(int jy=iy-1;jy<=iy+1;jy++){
				for(int jx=ix-1;jx<=ix+1;jx++){
					int jb = jz*bucket::nxy + jy*bucket::nx + jx;
					int j = bucket::first[jb];
					if(j == -1) continue;
					for(;;){
						Float v0 = pos[j].x - pos[i].x;
						Float v1 = pos[j].y - pos[i].y;
						Float v2 = pos[j].z - pos[i].z;
						Float dist2 = v0*v0 + v1*v1 + v2*v2;
						if(dist2 < r2){
							if(j!=i && type[j]!=GHOST){
								Float dist = sqrt(dist2);
								Float w = weight(dist, r);
								ni += w;
							}
						}
						j = bucket::next[j];
						if(j == -1) break;
					}
				}
			}
		}
		Float mi = dens[type[i]];
		Float pressure = (ni > n0) * (ni - n0) * A2 * mi;
		if(type[i] == FLUID){
			if(pos[i].x > 0.0 && 0.0 < pos[i].y && pos[i].y < 0.05) pressure += 0.1;
			if(pos[i].x > 0.0 && -0.05 < pos[i].y && pos[i].y < 0.0) pressure -= 0.1;
		}
		press[i] = pressure;
//		if(pressure != 0.0) PRINT(pressure);
	}
}

void simulation::calc_press_grad(){
#pragma omp parallel for schedule(dynamic,OMP_CHUNK_NUM)
	for(auto i=0;i<particle_number;i++){
		if(type[i] != FLUID) continue;
		sksat::math::vector<Float> Acc;
		Acc.x = Acc.y = Acc.z = 0.0;
		Float pre_min = press[i];
		int ix = static_cast<int>((pos[i].x - min.x)*bucket::width_inv) + 1;
		int iy = static_cast<int>((pos[i].y - min.y)*bucket::width_inv) + 1;
		int iz = static_cast<int>((pos[i].z - min.z)*bucket::width_inv) + 1;
		for(int jz=iz-1;jz<=iz+1;jz++){
			for(int jy=iy-1;jy<=iy+1;jy++){
				for(int jx=ix-1;jx<=ix+1;jx++){
					int jb = jz*bucket::nxy + jy*bucket::nx + jx;
					int j = bucket::first[jb];
					if(j == -1) continue;
					for(;;){
						Float v0 = pos[j].x - pos[i].x;
						Float v1 = pos[j].y - pos[i].y;
						Float v2 = pos[j].z - pos[i].z;
						Float dist2 = v0*v0 + v1*v1 + v2*v2;
						if(dist2 < r2){
							if(j!=i && type[j]!=GHOST){
								if(pre_min > press[j]) pre_min = press[j];
							}
						}
						j = bucket::next[j];
						if(j == -1) break;
					}
				}
			}
		}
		for(int jz=iz-1;jz<=iz+1;jz++){
			for(int jy=iy-1;jy<=iy+1;jy++){
				for(int jx=ix-1;jx<=ix+1;jx++){
					int jb = jz*bucket::nxy + jy*bucket::nx + jx;
					int j = bucket::first[jb];
					if(j == -1) continue;
					for(;;){
						Float v0 = pos[j].x - pos[i].x;
						Float v1 = pos[j].y - pos[i].y;
						Float v2 = pos[j].z - pos[i].z;
						Float dist2 = v0*v0 + v1*v1 + v2*v2;
						if(dist2 < r2){
							if(j!=i && type[j]!=GHOST){
								Float dist = sqrt(dist2);
								Float w = weight(dist, r);
								w *= (press[j] - pre_min)/dist2;
								Acc.x += v0*w;
								Acc.y += v1*w;
								Acc.z += v2*w;
							}
						}
						j = bucket::next[j];
						if(j == -1) break;
					}
				}
			}
		}
		acc[i] = Acc * dens_inv[FLUID] * A3;
	}
}

void simulation::move_particle(){
#pragma omp parallel for
	for(auto i=0;i<particle_number;i++){
		if(type[i] != FLUID) continue;
		vel[i] += acc[i] * dt;
		pos[i] += acc[i] * dt * dt;
		check_overflow(i);
//		if(check_overflow(i)) std::cout<<"move_particle: "<<acc[i].x<<std::endl;
		acc[i].x = acc[i].y = acc[i].z = 0.0;
	}
}

void simulation::move_body(){
	auto dtheta = rw::w*dt;
	for(auto i=0;i<particle_number;i++){
		if(type[i] != WALL) continue;
		auto s = sin(dtheta);
		auto c = cos(dtheta);
		auto x = (pos[i].x * c) - (pos[i].y * s);
		auto y = (pos[i].x * s) + (pos[i].y * c);
		pos[i].x = x;
		pos[i].y = y;
		check_overflow(i);
	}
	rw::theta += dtheta;
//	std::cout<<"theta: "<<rw::theta<<std::endl;
}

void simulation::write_file(const size_t &step, const Float &time){
	using std::endl;
	std::stringstream fname;
	fname << "out/"
		<< "output_"
		<< std::setfill('0')
		<< std::setw(5)
		<< file_number
		<< ".prof";
	std::ofstream f(fname.str());
	if(f.fail()) throw std::runtime_error("cannot open file.\nyou should make \'out\' dir.");

	f << time_step << endl
		<< time << endl
		<< particle_number << endl
		<< rw::r_in << " " << rw::r_out << endl
		<< rw::theta << " " << rw::w << endl;

	for(auto i=0;i<particle_number;i++){
		if(type[i] == GHOST) continue;
		f << type[i] << " "
			<< pos[i].x << " "
			<< pos[i].y << " "
			<< pos[i].z << " "
			<< vel[i].x << " "
			<< vel[i].y << " "
			<< vel[i].z << " "
			<< press[i] << endl;
	}

	file_number++;
}
