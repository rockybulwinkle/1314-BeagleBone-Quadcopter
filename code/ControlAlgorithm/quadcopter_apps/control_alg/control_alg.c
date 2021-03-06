#include <stdio.h>
#include <stdlib.h>
#include <prussdrv.h>
#include <pruss_intc_mapping.h>
#include <math.h>
#include <signal.h>
#include <fcntl.h>
#include <string.h>
#include <unistd.h>
#include <sys/mman.h>
#include <native/task.h>
#include <native/timer.h>

#include "control_alg.h"

RT_TASK demo_task;

volatile static void *pruDataMem;
volatile static signed int *pruDataMem_int;

void signal_handler(int sig){
	pruDataMem_int[0] = 0;
}

int get_set_point(set_point_t * goal, PID_t * PID_pitch, PID_t * PID_roll, PID_t * PID_yaw, comp_filter_t * theta_y){
	static int f = 0;
	static int num_since_last = 0;
	static int alive = 1;
	static int armed = 0;
	static double roll_goal = 0;
	static double pitch_goal = 0;
	static double yaw_goal = 0;
	static double z_goal = 0;
	static int zero_throttle_seen = 0;
	if (f == 0){
		f = open("/tmp/quadtempfs/BeagleQuad_ControlFifo.txt", O_NONBLOCK | O_RDONLY);
		if (f <=0 ){
			printf("couldn't open control file\n");
			pruDataMem_int[0] = 0;
		}
	}

	char buffer[256];
	int num_read = read(f, buffer, 256);
	buffer[num_read] = '\0';
	//message format:
	//throttle,aileron,elevator,rudder
	//throttle is negated
	//when increased:
	//aileron should roll right->set point increase
	//elevator should roll back->set point decrease
	//rudder should turn right->set point decrease
	int recieved_joy_update = 0;
	if (num_read>=1){
		num_since_last = 0;
		char * pch;
		pch = strtok(buffer, "\n");
		while(pch != NULL){
			//			printf("%s\n", pch);
			if (pch[0] == 'p' && pch[1] =='i' && pch[2] == 'd' && pch[3] == ':'){
				double rp, ri, rd, pp, pi, pd, yp, yi, yd;
				sscanf(pch, "pid: %lf %lf %lf %lf %lf %lf %lf %lf %lf", &pp, &pi, &pd, &rp, &ri, &rd, &yp, &yi, &yd);
				//printf("%lf %lf %lf %lf %lf %lf %lf %lf %lf\n", pp, pi, pd, rp, ri, rd, yp, yi, yd);

				PID_pitch->kP = pp;
				PID_pitch->kI = pi;
				PID_pitch->kD = pd;
				PID_roll->kP = rp;
				PID_roll->kI = ri;
				PID_roll->kD = rd;
				PID_yaw->kP = yp;
				PID_yaw->kI = yi;
				PID_yaw->kD = yd;

				//printf("updated pid: %d %d %d, %d %d %d, %d %d %d\n", rp, ri, rd, pp, pi, pd, yp, yi, yd);
			}else if (pch[0] == 'j' && pch[1] == 'o' && pch[2] == 'y' && pch[3] == ':'){
				recieved_joy_update = 1;

				sscanf(pch, "joy: %lf,%lf,%lf,%lf", &(z_goal), &(roll_goal), &(pitch_goal), &(yaw_goal));
				//printf("%f %f %f %f\n", z_goal, roll_goal, pitch_goal, yaw_goal);
				//printf("--\n");

				//pitch_goal += 5000;
				fflush(stdout);

				z_goal = (z_goal+32000)*.8;
				if (z_goal > 1000 && zero_throttle_seen){
					armed = 1;
				} else{
					armed = 0;
					zero_throttle_seen =1;
				}

				roll_goal/=-1500.0f;

				pitch_goal/=1500.0f;

				yaw_goal/=(11796*3); //turn 90 degrees per second
			}
			pch = strtok(NULL, "\n");
		}
		//goal->yaw = 0;
		//goal->pitch = 0;
		//goal->roll = 0;


		//printf("throttle: %f roll: %f pitch: %f yaw: %f\n", z_goal, roll_goal, pitch_goal, yaw_goal);
	}
	if (!recieved_joy_update){
		num_since_last+=1;
	}
	if (num_since_last >= 250){
		alive = 0;
	}
	if (!alive){
		pruDataMem_int[0] = 0;
		goal->yaw = 0;
		goal->pitch = 0;
		goal->yaw = 0;
		printf("controller timed out\n");
		exit(0);
	}
	
	if (fabs(yaw_goal) < .0001){ //arbitrary yaw rate
		goal->yaw = theta_y->th; //set the goal to the current theta if the user is not yawing.
	}

	goal->yaw += yaw_goal;
	goal->pitch = pitch_goal;
	goal->roll = roll_goal;
	goal->z = z_goal;


	usleep(0);
	return armed;


}

void init_PID(PID_t * PID_x, double kP, double kI, double kD){
	PID_x->kP = kP;
	PID_x->kI = kI;
	PID_x->I = 0;
	PID_x->kD = kD;
	PID_x->D = 0; 
}

void init_filter(comp_filter_t * comp_filter, double alpha, double beta, double g){
	comp_filter->alpha = alpha;
	comp_filter->beta = beta;
	comp_filter->g = g;
	comp_filter->th = 0;
}


void calculate_next_comp_filter(comp_filter_t * prev_data, double acc, double gyro, double dt){

	gyro = (gyro/GYRO_MAX_RAW)*GYRO_SENSITIVITY;
	double accel_angle = acc/prev_data->g;
	accel_angle = MAX(MIN(accel_angle, 1.0), -1.0);
	accel_angle = asin(accel_angle);
	prev_data->th = prev_data->alpha*(prev_data->th + gyro*dt) + prev_data->beta*accel_angle*RAD_TO_DEG;
}

void get_imu_frame(imu_data_t * imu_frame){
	rt_task_wait_period(NULL);
	//while(!pruDataMem_int[1] &&(pruDataMem_int[1000])){ // wait for pru to signal that new data is available
	//}


	imu_frame->x_a = (signed short)pruDataMem_int[2];
	imu_frame->y_a = (signed short)pruDataMem_int[3];
	imu_frame->z_a = (signed short)pruDataMem_int[4];
	imu_frame->x_g = (signed short)pruDataMem_int[5];
	imu_frame->y_g = (signed short)pruDataMem_int[6];
	imu_frame->z_g = (signed short)pruDataMem_int[7];
	imu_frame->sample_num = pruDataMem_int[12];

}

pru_pwm_frame_t * get_pwm_pointer(){
	pru_pwm_frame_t * pwm_frame = malloc(sizeof(pru_pwm_frame_t));
	pwm_frame->zero = &(pruDataMem_int[PWM_0_ADDRESS]);
	pwm_frame->one = &(pruDataMem_int[PWM_1_ADDRESS]);
	pwm_frame->two = &(pruDataMem_int[PWM_2_ADDRESS]);
	pwm_frame->three = &(pruDataMem_int[PWM_3_ADDRESS]);
	return pwm_frame;
}

void initialize_pru(){
	unsigned int ret;
	tpruss_intc_initdata pruss_intc_initdata = PRUSS_INTC_INITDATA;

	prussdrv_init ();
	/* Open PRU Interrupt */
	ret = prussdrv_open(PRU_EVTOUT_0);
	if (ret)
	{
		printf("prussdrv_open open failed\n");
		exit(ret);
	}

	/* Get the interrupt initialized */
	prussdrv_pruintc_init(&pruss_intc_initdata);

	//Initialize pointer to PRU data memory
	if (PRU_NUM == 0)
	{
		prussdrv_map_prumem (PRUSS0_PRU0_DATARAM, &pruDataMem);
	}
	else if (PRU_NUM == 1)
	{
		prussdrv_map_prumem (PRUSS0_PRU1_DATARAM, &pruDataMem);
	}  
	pruDataMem_int = (volatile signed int*) pruDataMem;
}
void start_pru(){
	pruDataMem_int[0] = 1;
	pruDataMem_int[13] = 0;
	prussdrv_exec_program (PRU_NUM, "./control_alg.bin");
}


void uninitialize_pru(){
	pruDataMem_int[0] = 0;
	pruDataMem_int[13] = 0;

	/* Wait until PRU0 has finished execution */
	printf("\tINFO: Waiting for HALT command.\r\n");
	prussdrv_pru_wait_event (PRU_EVTOUT_0);

	printf("\tINFO: PRU completed transfer.\r\n");
	prussdrv_pru_clear_event (PRU0_ARM_INTERRUPT);

	/* Disable PRU and close memory mapping*/
	prussdrv_pru_disable (PRU_NUM);
	prussdrv_exit ();


}

imu_data_t * get_calibration_data(){
	imu_data_t * calib_data = malloc(sizeof(imu_data_t));
	imu_data_t imu_data;
	calib_data->x_a = 0;
	calib_data->y_a = 0;
	calib_data->z_a = 0;
	calib_data->x_g = 0;
	calib_data->y_g = 0;
	calib_data->z_g = 0;

	int i;
	printf("clearing DLPF on imu..\n");
	for (i = 0; (i < 1000); i++){
		
		pruDataMem_int[13] = 0;
		get_imu_frame(&imu_data);
		pruDataMem_int[1] = 0;
	}

	for (i = 0; (i < CALIBRATION_SAMPLES); i++){
		
		pruDataMem_int[13] = 0;
		get_imu_frame(&imu_data);
		pruDataMem_int[1] = 0;

		calib_data->x_a += imu_data.x_a;
		calib_data->y_a += imu_data.y_a;
		calib_data->z_a += imu_data.z_a;

		calib_data->x_g += imu_data.x_g;
		calib_data->y_g += imu_data.y_g;
		calib_data->z_g += imu_data.z_g;
	}
	calib_data->x_a /= CALIBRATION_SAMPLES;
	calib_data->y_a /= CALIBRATION_SAMPLES;
	calib_data->z_a /= CALIBRATION_SAMPLES;
	calib_data->x_g /= CALIBRATION_SAMPLES;
	calib_data->y_g /= CALIBRATION_SAMPLES;
	calib_data->z_g /= CALIBRATION_SAMPLES;
	return calib_data;
}

void init_pwm(pwm_frame_t * pwm_frame){
	pwm_frame->zero = PWM_OFF;
	pwm_frame->one = PWM_OFF;
	pwm_frame->two = PWM_OFF;
	pwm_frame->three = PWM_OFF;
}

void output_pwm(pwm_frame_t * pwm_frame_next, pru_pwm_frame_t * pwm_out){
	*(pwm_out->zero) = pwm_frame_next->zero;
	*(pwm_out->one) = pwm_frame_next->one;
	*(pwm_out->two) = pwm_frame_next->two;
	*(pwm_out->three) = pwm_frame_next->three;
	pruDataMem_int[1] = 0;

}

void load_pid_values(PID_t * PID_pitch, PID_t * PID_roll, PID_t * PID_yaw, PID_t * PID_z){
	FILE * fp = fopen(PID_FILE, "r");
	if (fp == NULL){
		fprintf(stderr, "Couldn't open the %s :(\n", PID_FILE);
		exit(-1);
	}
	int pp, pi, pd, rp, ri, rd, yp, yi, yd, zp, zi, zd;
	int num_values = fscanf(fp, "%d, %d, %d\n%d, %d, %d\n%d, %d, %d\n%d, %d, %d", 
			&pp, &pi, &pd,
			&rp, &ri, &rd,
			&yp, &yi, &yd,
			&zp, &zi, &zd);
	if (num_values != 12 || num_values == EOF){
		perror("scanf");
		fprintf(stderr, "You are a failure.  Please reformat your %s file.\n", PID_FILE);
		exit(-1);
	}
	init_PID(PID_pitch, pp, pi, pd);
	init_PID(PID_roll, rp, ri, rd);
	init_PID(PID_yaw, yp, yi, yd);
	init_PID(PID_z, zp, zi, zd);
	fclose(fp);
}

void demo (void * arg)
{
        rt_task_set_periodic(NULL, TM_NOW, 5000000);
	


	comp_filter_t * theta_p = malloc(sizeof(comp_filter_t));
	comp_filter_t * theta_r = malloc(sizeof(comp_filter_t));
	comp_filter_t * theta_y = malloc(sizeof(comp_filter_t));
	pwm_frame_t * next_pwm = malloc(sizeof(comp_filter_t));
	double * z_pos = malloc(sizeof(double)); 
	double * z_vel = malloc(sizeof(double));
	PID_t * PID_pitch = malloc(sizeof(PID_t));
	PID_t * PID_roll = malloc(sizeof(PID_t));
	PID_t * PID_yaw = malloc(sizeof(PID_t));
	PID_t * PID_z = malloc(sizeof(PID_t));
	set_point_t * goal = malloc(sizeof(set_point_t));
	set_point_t * cf = malloc(sizeof(set_point_t));

	FILE * response_log = fopen("system_response.csv", "w");
	if (response_log == NULL){
		fprintf(stderr, "Couldn't open response.csv");
		exit(-1);
	}
	fprintf(response_log, "bias,pitch,cf_pitch,roll,cf_roll,yaw,cf_yaw,z,m0,m1,m2,m3\n");
	load_pid_values(PID_pitch, PID_roll, PID_yaw, PID_z);

	initialize_pru();
	pru_pwm_frame_t * pwm_out = get_pwm_pointer();
	init_pwm(next_pwm);
	output_pwm(next_pwm, pwm_out);
	start_pru();

	imu_data_t * calib_data;
	FILE * calib_file = fopen("./cal.txt", "r");
	if (calib_file == NULL){
		printf("generating new calibration data\n");
		calib_data = get_calibration_data();
		calib_file = fopen("./cal.txt", "w");
		if (calib_file == NULL){
			fprintf(stderr, "couldn't open cal.txt\n");
		} else{
			fprintf(calib_file, "%f,%f,%f,%f,%f,%f\n", calib_data->x_a, calib_data->y_a, calib_data->z_a, calib_data->x_g, calib_data->y_g, calib_data->z_g);
			fclose(calib_file);
		}

		uninitialize_pru();
		exit(-1);
	} else{
		calib_data = malloc(sizeof(imu_data_t));
		fscanf(calib_file, "%lf,%lf,%lf,%lf,%lf,%lf\n", &(calib_data->x_a), &(calib_data->y_a), &(calib_data->z_a), &(calib_data->x_g), &(calib_data->y_g), &(calib_data->z_g));
		//		printf("cal data: %f, %f, %f, %f, %f, %f\n", calib_data->x_a, calib_data->y_a, calib_data->z_a, calib_data->x_g, calib_data->y_g, calib_data->z_g);
		fclose(calib_file);
	}

//	signal(SIGINT, signal_handler);
	printf("check calibration data for sanity: %f, %f, %f, %f, %f, %f\n", calib_data->x_a, calib_data->y_a, calib_data->z_a, calib_data->x_g, calib_data->y_g, calib_data->z_g);

	*z_pos = 0;
	*z_vel = 0;

	imu_data_t * imu_frame = malloc(sizeof(imu_data_t));

	init_filter(theta_p, ALPHA, BETA, G);
	init_filter(theta_r, ALPHA, BETA, G);
	init_filter(theta_y, 1, 0, G);


	double bias = 0;
	int count = 0;
	int time = 0;
	goal->z = 0;
	goal->pitch = 0;
	goal->roll = 0;
	goal->yaw = 0;
	while(pruDataMem_int[0] != 0){
		pruDataMem_int[13] = 0;

		get_imu_frame(imu_frame); //called because this basically controls our timesteps
		count++;
		if (imu_frame->sample_num != count){
			if (imu_frame->sample_num-count+1){
				printf("Skipped %d frames\n", imu_frame->sample_num-count+1);
			}
			count = imu_frame->sample_num;
		}

		calibrate_imu_frame(imu_frame, calib_data);
		if ((count % 20) == 0){
			printf ("I'm still here!\n");

			/*
			printf("bias: % 03f", bias);
			printf(", pitch: % 03.5f, cf_pitch: % 03.5f", theta_p->th, cf->pitch);
			printf(", roll: % 03.5f, cf_roll: % 03.5f", theta_r->th, cf->roll);
			printf(", yaw: % 03.5f, cf->yaw: % 03.5f", theta_y->th, cf->yaw);
			printf(", m0: %d, m1: %d, m2: %d, m3: %d\n", next_pwm->zero, next_pwm->one, next_pwm->two, next_pwm->three);
			*/
			printf("I: %f D: %f pitch: %f\n", PID_roll->I, PID_roll->D, theta_r->th);
			//printf("goal: pitch: %f roll: %f yaw: %f z: %f\n", goal->pitch, goal->roll, goal->yaw, bias);
			//printf("goalyaw: %f\n", goal->yaw);

		}


		if (get_set_point(goal, PID_pitch, PID_roll, PID_yaw, theta_y)){
			bias = goal->z;
			if (bias < BIAS_MAX){
				//bias = BIAS_MAX/(1.0f+exp(-0.01656695d*time+6.2126d));
			} else{
				//bias = BIAS_MAX;
				bias = BIAS_MAX;
			}

			filter_loop(imu_frame, theta_p, theta_r, theta_y, z_pos, z_vel);
			calculate_next_pwm(next_pwm, theta_p, theta_r, theta_y, z_pos, z_vel, PID_pitch, PID_roll, PID_yaw, PID_z, goal, bias, cf, imu_frame);
			output_pwm(next_pwm, pwm_out);


			//fprintf(response_log, "%f,%3.5f,%3.5f,%3.5f,%3.5f,%3.5f,%3.5f,%3.5f,\t%d,%d,%d,%d\n", bias, theta_p->th,cf->pitch, theta_r->th,cf->roll, theta_y->th, cf->yaw, *z_vel, next_pwm->zero, next_pwm->one, next_pwm->two, next_pwm->three);
			//		printf("x_g: % 05.5f, x_a: % 05.5f, y_g: % 05.5f, y_a: % 05.5f, z_g: % 05.5f, z_a: % 05.5f\n", imu_frame->x_g, imu_frame->x_a, imu_frame->y_g, imu_frame->y_a, imu_frame->z_g, imu_frame->z_a);
			time += 1;
		} else{ // not armed
			time = 0;
			init_pwm(next_pwm);
			output_pwm(next_pwm, pwm_out);
			PID_pitch->I = 0;
			PID_pitch->D = 0;
			PID_roll->I = 0;
			PID_roll->D = 0;
			PID_yaw->I = 0;
			PID_yaw->D = 0;
			PID_z->I = 0;
			PID_z->D = 0;
			goal->z = 0;
			goal->roll = 0;
			goal->yaw = 0;
			goal->pitch = 0;
			theta_y->th = 0;
		}
		
	}
	printf("exiting...\n");

	uninitialize_pru();
	fclose(response_log);
	free(theta_p);
	free(theta_r);
	free(theta_y);
	free(z_pos);
	free(z_vel);
	free(calib_data);
	free(PID_pitch);
	free(PID_roll);
	free(PID_yaw);
	free(PID_z);
	free(goal);
	free(pwm_out);
	free(next_pwm);	
	free(cf);
	free(imu_frame);
	//return(0);
}


int main(int argc, char* argv[])
{
        signal(SIGTERM, signal_handler);
        signal(SIGINT, signal_handler);

        /* Avoids memory swapping for this program */
        mlockall(MCL_CURRENT|MCL_FUTURE);

        /*
         * Arguments: &task,
         *            name,
         *            stack size (0=default),
         *            priority,
         *            mode (FPU, start suspended, ...)
         */
        rt_task_create(&demo_task, "trivial", 0, 99, 0);

        /*
         * Arguments: &task,
         *            task function,
         *            function argument
         */
        rt_task_start(&demo_task, &demo, NULL);

        pause();

        rt_task_delete(&demo_task);
}


void calibrate_imu_frame(imu_data_t * imu_frame, imu_data_t * calib_data){
	imu_frame->x_a -= calib_data->x_a;
	imu_frame->y_a -= calib_data->y_a;
	imu_frame->z_a -= calib_data->z_a + 1;

	imu_frame->x_g -= calib_data->x_g;
	imu_frame->y_g -= calib_data->y_g;
	imu_frame->z_g -= calib_data->z_g;
}

void filter_loop(imu_data_t * imu_frame, comp_filter_t * theta_p, comp_filter_t * theta_r, comp_filter_t * theta_y, double * z_pos, double * z_vel){
	double temp_z_acc = imu_frame->z_a*fabs(cos(theta_p->th/RAD_TO_DEG)*cos(theta_r->th/RAD_TO_DEG));
	temp_z_acc = ((temp_z_acc/32768.0f)*8.0f);
	*z_vel += temp_z_acc;

	//forward is +y is towards the ethernet port
	//right is +x is header P9
	//up is +z is the vector pointing from the cape to the beaglebone
	calculate_next_comp_filter(theta_p, -imu_frame->y_a, imu_frame->x_g, DT);
	calculate_next_comp_filter(theta_r, imu_frame->x_a, imu_frame->y_g, DT);
	calculate_next_comp_filter(theta_y, -imu_frame->z_a, -imu_frame->z_g, DT);
}

void calculate_next_pwm(pwm_frame_t * next_pwm, comp_filter_t * theta_p, comp_filter_t * theta_r, comp_filter_t * theta_y, double * z_pos, double * z_vel, PID_t * PID_pitch, PID_t * PID_roll, PID_t * PID_yaw, PID_t * PID_z, set_point_t * goal, int bias, set_point_t * cf, imu_data_t * imu_data){
	double d_pitch = PID_loop(goal->pitch, PID_pitch, theta_p->th);
	double d_roll = PID_loop(goal->roll, PID_roll, theta_r->th);
	double d_yaw = PID_loop(goal->yaw, PID_yaw, theta_y->th);
	double d_z = PID_loop(goal->z, PID_z, *z_vel);
	cf->roll = d_roll;
	cf->pitch = d_pitch;
	cf->yaw = d_yaw;

	d_z = 0;//FIXME
	//
	//d_yaw = 0;
	//d_roll = 0;
	//d_pitch = 0;

	next_pwm->zero = d_pitch + d_roll + d_yaw - d_z + PWM_MIN;
	next_pwm->one = -d_pitch + d_roll - d_yaw - d_z + PWM_MIN;
	next_pwm->two = -d_pitch - d_roll + d_yaw - d_z + PWM_MIN;
	next_pwm->three = d_pitch - d_roll - d_yaw - d_z + PWM_MIN;

	next_pwm->zero = next_pwm->zero * MULT0 + bias + BIAS0;
	next_pwm->one = next_pwm->one * MULT1 + bias + BIAS1;
	next_pwm->two = next_pwm->two * MULT2 + bias + BIAS2;
	next_pwm->three = next_pwm->three * MULT3 + bias + BIAS3;

	next_pwm->zero = MIN(PWM_MAX, MAX(PWM_MIN,next_pwm->zero));
	next_pwm->one = MIN(PWM_MAX, MAX(PWM_MIN,next_pwm->one));
	next_pwm->two = MIN(PWM_MAX, MAX(PWM_MIN,next_pwm->two));
	next_pwm->three = MIN(PWM_MAX, MAX(PWM_MIN,next_pwm->three));
}

double PID_loop(double goal, PID_t * PID_x, double value){
	double P, I, D, delta_error;
	delta_error = goal - value;

	P = delta_error;
	I = (PID_x->I + PID_x->kI*delta_error*DT);
	I = MIN(MAX_I, MAX(MIN_I,I));
	//PID_x->I = I*.998;
	PID_x->I = I;
	D = PID_x->D - value;
	PID_x->D = value;

	return P*PID_x->kP + I + D*PID_x->kD/DT;
}





