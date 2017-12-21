#include <iostream>
#include <fstream>
#include <string>
#include <sstream>
#include <vector>
#include <iomanip>
#include <cmath>
#include <sksat/math/vector.hpp>
#include "common.hpp"

namespace simulation {
	size_t time_step = 0;
	Float time=.0, dt=0.001, finish_time=2.0;

	size_t dim=2;
	size_t particle_number=0;
	std::vector< sksat::math::vector<Float> > acc, vel, pos;
	std::vector<int> type;

	size_t output_interval = 20;
	size_t file_number = 0;

	void main_loop();
	void move_particle();
	void move_body();
	void load_file(const std::string &fname);
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

	std::cout<<" *** START SIMULATION *** "<<std::endl;
	simulation::main_loop();
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

	acc.reserve(particle_number);
	vel.reserve(particle_number);
	pos.reserve(particle_number);
	type.reserve(particle_number);

	for(auto i=0;i<particle_number;i++){
		f >> type[i]
			>> pos[i].x
			>> pos[i].y
			>> vel[i].x
			>> vel[i].y;
		if(f.eof()){
			std::cerr<<"stop: "<<i<<std::endl;
			throw std::runtime_error("");
		}
	}
}

void simulation::main_loop(){
	if(time_step == 0)
		write_file(0, time);
	while(true){
		move_particle();
		move_body();

		time_step++;
		time += dt;
		if( (time_step % output_interval) == 0 ){
			std::cout
				<< "time step: " << std::setw(5) << time_step << "  "
				<< "time: " << std::setw(5) << time << "  "
				<< "particle number: " << particle_number
				<< std::endl;
			write_file(time_step, time);
		}
		if(time >= finish_time) break;
	}
}

void simulation::move_particle(){
	for(auto i=0;i<particle_number;i++){
		if(type[i] != FLUID) continue;
		vel[i].x += acc[i].x;
		vel[i].y += acc[i].y;
		pos[i].x += vel[i].x;
		pos[i].y += vel[i].y;
	}
}

void simulation::move_body(){
	for(auto i=0;i<particle_number;i++){
		if(type[i] != WALL) continue;
		auto dtheta = rw::w *dt;
		auto s = sin(dtheta);
		auto c = cos(dtheta);
		auto x = (pos[i].x * c) - (pos[i].y * s);
		auto y = (pos[i].x * s) + (pos[i].y * c);
		rw::theta += dtheta;
		pos[i].x = x;
		pos[i].y = y;
	}
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
		f << type[i] << " "
			<< pos[i].x << " "
			<< pos[i].y << " "
			<< vel[i].x << " "
			<< vel[i].y << endl;
	}

	file_number++;
}
