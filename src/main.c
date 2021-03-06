/*
 * Copyright 2019 IBM
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <sys/time.h>
#include <unistd.h>

#include "verbose.h"

#include "scheduler.h"
#include "kernels_api.h"
#include "sim_environs.h"
#include "getopt.h"

#define TIME

#define get_mb_holdoff 10  // usec

char h264_dict[256]; 
char cv_dict[256]; 
char rad_dict[256];
char vit_dict[256];

bool_t bypass_h264_functions = false; // This is a global-disable of executing H264 execution functions...



// These are now defined in terms of measurements (recorded in macro definitions, in scheduler.h)

// FFT has 2 profiles depending on input size (1k or 16k samples)
//   CPU     FFT     VIT        CV         NONE
uint64_t fft_profile[2][NUM_ACCEL_TYPES] = {
//   CPU        FFT        VIT        CV        NONE
  { 23000, usecHwrFFT0, ACINFPROF, ACINFPROF, ACINFPROF},  //  1k-sample FFT
  {600000, usecHwrFFT1, ACINFPROF, ACINFPROF, ACINFPROF}}; // 16k-sample FFT

// Viterbi has 4 profiles, depending on input size
uint64_t vit_profile[4][NUM_ACCEL_TYPES] = {
//    CPU        FFT        VIT          CV        NONE
  { 120000,  ACINFPROF, usecHwrVIT0, ACINFPROF, ACINFPROF},  // short-message Vit
  {1700000,  ACINFPROF, usecHwrVIT1, ACINFPROF, ACINFPROF},  // medium-message Vit
  {3400000,  ACINFPROF, usecHwrVIT2, ACINFPROF, ACINFPROF},  // long-message Vit
  {4800000,  ACINFPROF, usecHwrVIT3, ACINFPROF, ACINFPROF}}; // max-message Vit

uint64_t cv_profile[NUM_ACCEL_TYPES]  = {
//    CPU       FFT        VIT         CV       NONE
  ACINFPROF, ACINFPROF, ACINFPROF, usecHwrCV, ACINFPROF};


bool_t all_obstacle_lanes_mode = false;
bool_t no_crit_cnn_task = false;
unsigned time_step;
unsigned pandc_repeat_factor = 1;
unsigned task_size_variability;

void print_usage(char * pname) {
  printf("Usage: %s <OPTIONS>\n", pname);
  printf(" OPTIONS:\n");
  printf("    -h         : print this helpful usage info\n");
  printf("    -o         : print the Visualizer output traace information during the run\n");
  printf("    -R <file>  : defines the input Radar dictionary file <file> to use\n");
  printf("    -V <file>  : defines the input Viterbi dictionary file <file> to use\n");
  printf("    -C <file>  : defines the input CV/CNN dictionary file <file> to use\n");
  //printf("    -H <file>  : defines the input H264 dictionary file <file> to use\n");
  //printf("    -b         : Bypass (do not use) the H264 functions in this run.\n");
  printf("    -s <N>     : Sets the max number of time steps to simulate\n");
 #ifdef USE_SIM_ENVIRON
  printf("    -r <N>     : Sets the rand random number seed to N\n");
  printf("    -A         : Allow obstacle vehciles in All lanes (otherwise not in left or right hazard lanes)\n");
  printf("    -W <wfile> : defines the world environment parameters description file <wfile> to use\n");
 #else
  printf("    -t <trace> : defines the input trace file <trace> to use\n");
 #endif
  printf("    -p <N>     : defines the plan-and-control repeat factor (calls per time step -- default is 1)\n");
  printf("    -f <N>     : defines which Radar Dictionary Set is used for Critical FFT Tasks\n");
  printf("               :      Each Set of Radar Dictionary Entries Can use a different sample size, etc.\n");
  
  printf("    -N <N>     : Adds <N> additional (non-critical) CV/CNN tasks per time step.\n");
  printf("    -D <N>     : Delay (in usec) of CPU CV Tasks (faked execution)\n");
 #ifdef FAKE_HW_CV
  printf("    -d <N>     : Delay (in usec) of HWR CV Tasks (faked execution)\n");
 #endif
  printf("    -F <N>     : Adds <N> additional (non-critical) FFT tasks per time step.\n");
  printf("    -v <N>     : defines Viterbi message size:\n");
  printf("               :      0 = Short messages (4 characters)\n");
  printf("               :      1 = Medium messages (500 characters)\n");
  printf("               :      2 = Long messages (1000 characters)\n");
  printf("               :      3 = Max-sized messages (1500 characters)\n");
  printf("    -M <N>     : Adds <N> additional (non-critical) Viterbi message tasks per time step.\n");
  printf("    -S <N>     : Task-Size Variability: Varies the sizes of input tasks where appropriate\n");
  printf("               :      0 = No variability (e.g. all messages same size, etc.)\n");
  printf("    -u <N>     : Sets the hold-off usec for checks on work in the scheduler queue\n");
  printf("               :   This reduces the busy-spin-loop rate for the scheduler thread\n");
  printf("    -P <N>     : defines the Scheduler Accelerator Selection Policy:\n");
  printf("               :      0 = Select_Accelerator_Type_And_Wait\n");
  printf("               :      1 = Fastest_to_Slowest_First_Available\n");
  printf("               :      2 = Fastest_Finish_Time_First\n");
  printf("               :      3 = Fastest_Finish_Time_First_Queued\n");
}


// This is just a call-through to the scheduler routine, but we can also print a message here...
//  This SHOULD be a routine that "does the right work" for a given task, and then releases the MetaData Block
void base_release_metadata_block(task_metadata_block_t* mb)
{
  TDEBUG(printf("Releasing Metadata Block %u : Task %s %s from Accel %s %u\n", mb->block_id, task_job_str[mb->job_type], task_criticality_str[mb->crit_level], accel_type_str[mb->accelerator_type], mb->accelerator_id));
  free_task_metadata_block(mb);
  // Thread is done -- We shouldn't need to do anything else -- when it returns from its starting function it should exit.
}

void radar_release_metadata_block(task_metadata_block_t* mb)
{
  TDEBUG(printf("Releasing Metadata Block %u : Task %s %s from Accel %s %u\n", mb->block_id, task_job_str[mb->job_type], task_criticality_str[mb->crit_level], accel_type_str[mb->accelerator_type], mb->accelerator_id));
  // Call this so we get final stats (call-time)
  distance_t distance = finish_execution_of_rad_kernel(mb);

  free_task_metadata_block(mb);
  // Thread is done -- We shouldn't need to do anything else -- when it returns from its starting function it should exit.
}


	 
int main(int argc, char *argv[])
{
  vehicle_state_t vehicle_state;
  label_t label;
  distance_t distance;
  message_t message;
 #ifdef USE_SIM_ENVIRON
  char* world_desc_file_name = "default_world.desc";
 #else
  char* trace_file = "";
 #endif
  int opt;

  rad_dict[0] = '\0';
  vit_dict[0] = '\0';
  cv_dict[0]  = '\0';
  h264_dict[0]  = '\0';

  unsigned additional_cv_tasks_per_time_step = 0;
  unsigned additional_fft_tasks_per_time_step = 0;
  unsigned additional_vit_tasks_per_time_step = 0;
  unsigned max_additional_tasks_per_time_step = 0;

  //printf("SIZEOF pthread_t : %lu\n", sizeof(pthread_t));
  
  // put ':' in the starting of the
  // string so that program can
  // distinguish between '?' and ':'
  while((opt = getopt(argc, argv, ":hcAbot:v:s:r:W:R:V:C:H:f:p:F:M:P:S:N:d:D:u:")) != -1) {
    switch(opt) {
    case 'h':
      print_usage(argv[0]);
      exit(0);
    case 'A':
      all_obstacle_lanes_mode = true;
      break;
    case 'c':
      no_crit_cnn_task= true;
      break;
    case 'o':
      output_viz_trace = true;
      break;
    case 'R':
      snprintf(rad_dict, 255, "%s", optarg);
      break;
    case 'C':
      snprintf(cv_dict, 255, "%s", optarg);
      break;
    case 'H':
      snprintf(h264_dict, 255, "%s", optarg);
      break;
    case 'V':
      snprintf(vit_dict, 255, "%s", optarg);
      break;
    case 'b':
      bypass_h264_functions = true;
      break;
    case 'u':
      scheduler_holdoff_usec = atoi(optarg);
      break;
    case 's':
      max_time_steps = atoi(optarg);
      break;
    case 'p':
      pandc_repeat_factor = atoi(optarg);
      break;
    case 'f':
      crit_fft_samples_set = atoi(optarg);
      break;
    case 'r':
     #ifdef USE_SIM_ENVIRON
      rand_seed = atoi(optarg);
     #endif
      break;
    case 't':
     #ifndef USE_SIM_ENVIRON
      trace_file = optarg;
     #endif
      break;
    case 'v':
      vit_msgs_size = atoi(optarg);
      if (vit_msgs_size >= VITERBI_MSG_LENGTHS) {
	printf("ERROR: Specified viterbi message size (%u) is larger than max (%u) : from the -v option\n", vit_msgs_size, VITERBI_MSG_LENGTHS);
	print_usage(argv[0]);
	exit(-1);
      }
      break;
    case 'S':
      task_size_variability = atoi(optarg);
      break;
    case 'W':
     #ifdef USE_SIM_ENVIRON
      world_desc_file_name = optarg;
     #endif
      break;
    case 'F':
      additional_fft_tasks_per_time_step = atoi(optarg);
      break;
    case 'M':
      additional_vit_tasks_per_time_step = atoi(optarg);
      break;
    case 'N':
      additional_cv_tasks_per_time_step = atoi(optarg);
      break;
    case 'P':
      global_scheduler_selection_policy = atoi(optarg);
      break;

    case 'd':
     #ifdef FAKE_HW_CV
      cv_fake_hwr_run_time_in_usec = atoi(optarg);
     #else
      printf("ERROR : I don't understand option '-d'\n");
      print_usage(argv[0]);
      exit(-1);
     #endif
      break;
    case 'D':
      cv_cpu_run_time_in_usec = atoi(optarg);
      break;

    case ':':
      printf("option needs a value\n");
      break;
    case '?':
      printf("unknown option: %c\n", optopt);
    break;
    }
  }

  // optind is for the extra arguments
  // which are not parsed
  for(; optind < argc; optind++){
    printf("extra arguments: %s\n", argv[optind]);
  }

  if (pandc_repeat_factor == 0) {
    printf("ERROR - Plan-and-Control repeat factor must be >= 1 : %u specified (with '-p' option)\n", pandc_repeat_factor);
    print_usage(argv[0]);
    exit(-1);
  }

  printf("Run using Scheduler Policy %u with %u CPU accel %u HWR FFT %u HWR VIT and %u HWR CV and hold-off %u\n",
	  global_scheduler_selection_policy, NUM_CPU_ACCEL, NUM_FFT_ACCEL, NUM_VIT_ACCEL, NUM_CV_ACCEL, scheduler_holdoff_usec);
 #ifdef HW_FFT
  printf("Run has enabled Hardware-FFT : Device base is %s\n", FFT_DEV_BASE);
 #else 
  printf("Run is using ONLY-CPU-FFT\n");
 #endif
 #ifdef HW_VIT
  printf("Run has enabled Hardware-Viterbi : Device base is %s\n", VIT_DEV_BASE);
 #else 
  printf("Run is using ONLY-CPU-Viterbi\n");
 #endif
  {
    char* cv0_txt[3] = { "ONLY-CPU-", "CPU-And-", "ONLY-"};
    char* cv1_txt[3] = { "", "Fake-", "Hardware-" };
    int i = 0;
    int is = 0;
    int ie = 0;
   #ifdef HW_ONLY_CV
    i = 2;
   #endif
   #ifdef FAKE_HW_CV
    if (i == 0) { i = 1; }
    is = 1;
    ie = 2;
   #endif
   #ifdef HW_CV
    if (i == 0) { i = 1; }
    if (is == 0) { is = 2; }
    ie = 2;
   #endif
    printf("Run is using %s", cv0_txt[i]);
    for (int ix = is; ix <= ie; ix++ ){
      printf("%s", cv1_txt[ix]);
    }
    printf("CV with no-crit-CV = %u\n", no_crit_cnn_task);
  }
 #ifndef HW_ONLY_CV
  printf(" with cv_cpu_run_time_in_usec set to %u\n", cv_cpu_run_time_in_usec);
 #endif
 #ifdef FAKE_HW_CV
  printf("  and cv_fake_hwr_run_time_in_usec set to %u\n", cv_fake_hwr_run_time_in_usec);
 #endif
  
  printf("Using Plan-And-Control repeat factor %u\n", pandc_repeat_factor);
  printf("Using Radar Dictionary samples set %u for the critical FFT tasks\n", crit_fft_samples_set);
  printf("Using viterbi message size %u = %s\n", vit_msgs_size, vit_msgs_size_str[vit_msgs_size]);
  printf("Using task-size variability behavior %u\n", task_size_variability);
  printf("Using %u maximum time steps (simulation)\n", max_time_steps);
 #ifdef USE_SIM_ENVIRON
  printf("Using world description file: %s\n", world_desc_file_name);
  printf("Using random seed of %u\n", rand_seed);
 #else
  printf("Using trace file: %s\n", trace_file);
 #endif
      
  if (rad_dict[0] == '\0') {
    sprintf(rad_dict, "traces/norm_radar_all_dictionary.dfn");
  }
  if (vit_dict[0] == '\0') {
    sprintf(vit_dict, "traces/vit_dictionary.dfn");
  }
  if (cv_dict[0] == '\0') {
    sprintf(cv_dict, "traces/objects_dictionary.dfn");
  }
  if (h264_dict[0] == '\0') {
    sprintf(h264_dict, "traces/h264_dictionary.dfn");
  }

  printf("\nDictionaries:\n");
  printf("   CV/CNN : %s\n", cv_dict);
  printf("   Radar  : %s\n", rad_dict);
  printf("   Viterbi: %s\n", vit_dict);

  printf("\n There are %u additional FFT, %u addtional Viterbi and %u Additional CV/CNN tasks per time step\n", additional_fft_tasks_per_time_step, additional_vit_tasks_per_time_step, additional_cv_tasks_per_time_step);
  max_additional_tasks_per_time_step = additional_fft_tasks_per_time_step;
  if (additional_vit_tasks_per_time_step > max_additional_tasks_per_time_step) {
    max_additional_tasks_per_time_step = additional_vit_tasks_per_time_step;
  }
  if (additional_cv_tasks_per_time_step > max_additional_tasks_per_time_step) {
    max_additional_tasks_per_time_step = additional_cv_tasks_per_time_step;
  }
  /* We plan to use three separate trace files to drive the three different kernels
   * that are part of mini-ERA (CV, radar, Viterbi). All these three trace files
   * are required to have the same base name, using the file extension to indicate
   * which kernel the trace corresponds to (cv, rad, vit).
   */
  /* if (argc != 2) */
  /* { */
  /*   printf("Usage: %s <trace_basename>\n\n", argv[0]); */
  /*   printf("Where <trace_basename> is the basename of the trace files to load:\n"); */
  /*   printf("  <trace_basename>.cv  : trace to feed the computer vision kernel\n"); */
  /*   printf("  <trace_basename>.rad : trace to feed the radar (FFT-1D) kernel\n"); */
  /*   printf("  <trace_basename>.vit : trace to feed the Viterbi decoding kernel\n"); */

  /*   return 1; */
  /* } */


  char cv_py_file[] = "../cv/keras_cnn/lenet.py";

  printf("Doing initialization tasks...\n");
  initialize_scheduler();

#ifndef USE_SIM_ENVIRON
  /* Trace Reader initialization */
  if (!init_trace_reader(trace_file))
  {
    printf("Error: the trace reader couldn't be initialized properly -- check the '-t' option.\n");
    print_usage(argv[0]);
    return 1;
  }
#endif

  /* Kernels initialization */
  /**if (bypass_h264_functions) {
    printf("Bypassing the H264 Functionality in this run...\n");
  } else {
    printf("Initializing the H264 kernel...\n");
    if (!init_h264_kernel(h264_dict))
      {
	printf("Error: the H264 decoding kernel couldn't be initialized properly.\n");
	return 1;
      }
      }**/

  printf("Initializing the CV kernel...\n");
  if (!init_cv_kernel(cv_py_file, cv_dict))
  {
    printf("Error: the computer vision kernel couldn't be initialized properly.\n");
    return 1;
  }
  printf("Initializing the Radar kernel...\n");
  if (!init_rad_kernel(rad_dict))
  {
    printf("Error: the radar kernel couldn't be initialized properly.\n");
    return 1;
  }
  printf("Initializing the Viterbi kernel...\n");
  if (!init_vit_kernel(vit_dict))
  {
    printf("Error: the Viterbi decoding kernel couldn't be initialized properly.\n");
    return 1;
  }

  if (crit_fft_samples_set >= num_radar_samples_sets) {
    printf("ERROR : Selected FFT Tasks from Radar Dictionary Set %u but there are only %u sets in the dictionary %s\n", crit_fft_samples_set, num_radar_samples_sets, rad_dict);
    print_usage(argv[0]);
    cleanup_and_exit(-1);
  }
    
  if (global_scheduler_selection_policy > NUM_SELECTION_POLICIES) {
    printf("ERROR : Selected Scheduler Policy (%u) is larger than number of policies (%u)\n", global_scheduler_selection_policy, NUM_SELECTION_POLICIES);
    print_usage(argv[0]);
    cleanup_and_exit(-1);
  }
  printf("Scheduler is using Policy %u : %s\n", global_scheduler_selection_policy, scheduler_selection_policy_str[global_scheduler_selection_policy]);
  
  /* We assume the vehicle starts in the following state:
   *  - Lane: center
   *  - Speed: 50 mph
   */
  vehicle_state.active  = true;
  vehicle_state.lane    = center;
  vehicle_state.speed   = 50;
  DEBUG(printf("Vehicle starts with the following state: active: %u lane %u speed %.1f\n", vehicle_state.active, vehicle_state.lane, vehicle_state.speed));

  #ifdef USE_SIM_ENVIRON
  // In simulation mode, we could start the main car is a different state (lane, speed)
  init_sim_environs(world_desc_file_name, &vehicle_state);
  #endif

/*** MAIN LOOP -- iterates until all the traces are fully consumed ***/
  time_step = 0;
 #ifdef TIME
  struct timeval stop_prog, start_prog;

  struct timeval stop_iter_rad, start_iter_rad;
  struct timeval stop_iter_vit, start_iter_vit;
  struct timeval stop_iter_cv , start_iter_cv;
  struct timeval stop_iter_h264 , start_iter_h264;

  uint64_t iter_rad_sec = 0LL;
  uint64_t iter_vit_sec = 0LL;
  uint64_t iter_cv_sec  = 0LL;
  uint64_t iter_h264_sec  = 0LL;

  uint64_t iter_rad_usec = 0LL;
  uint64_t iter_vit_usec = 0LL;
  uint64_t iter_cv_usec  = 0LL;
  uint64_t iter_h264_usec  = 0LL;

  struct timeval stop_exec_rad, start_exec_rad;
  struct timeval stop_exec_vit, start_exec_vit;
  struct timeval stop_exec_cv , start_exec_cv;
  struct timeval stop_exec_h264 , start_exec_h264;

  uint64_t exec_rad_sec = 0LL;
  uint64_t exec_vit_sec = 0LL;
  uint64_t exec_cv_sec  = 0LL;
  uint64_t exec_h264_sec  = 0LL;

  uint64_t exec_rad_usec = 0LL;
  uint64_t exec_vit_usec = 0LL;
  uint64_t exec_cv_usec  = 0LL;
  uint64_t exec_h264_usec  = 0LL;

  uint64_t exec_get_rad_sec = 0LL;
  uint64_t exec_get_vit_sec = 0LL;
  uint64_t exec_get_cv_sec  = 0LL;
  uint64_t exec_get_h264_sec  = 0LL;

  uint64_t exec_get_rad_usec = 0LL;
  uint64_t exec_get_vit_usec = 0LL;
  uint64_t exec_get_cv_usec  = 0LL;
  uint64_t exec_get_h264_usec  = 0LL;

  struct timeval stop_exec_pandc , start_exec_pandc;
  uint64_t exec_pandc_sec  = 0LL;
  uint64_t exec_pandc_usec  = 0LL;

  struct timeval stop_wait_all_crit, start_wait_all_crit;
  uint64_t wait_all_crit_sec = 0LL;
  uint64_t wait_all_crit_usec = 0LL;
 #endif // TIME

  printf("Starting the main loop...\n");
  /* The input trace contains the per-epoch (time-step) input data */
 #ifdef TIME
  gettimeofday(&start_prog, NULL);
  init_accelerators_in_use_interval(start_prog);
 #endif
  
 #ifdef USE_SIM_ENVIRON
  DEBUG(printf("\n\nTime Step %d\n", time_step));
  while (iterate_sim_environs(vehicle_state))
 #else //TRACE DRIVEN MODE

  read_next_trace_record(vehicle_state);
  while ((time_step < max_time_steps) && (!eof_trace_reader()))
 #endif
  {
    DEBUG(printf("Vehicle_State: Lane %u %s Speed %.1f\n", vehicle_state.lane, lane_names[vehicle_state.lane], vehicle_state.speed));

    /* The computer vision kernel performs object recognition on the
     * next image, and returns the corresponding label. 
     * This process takes place locally (i.e. within this car).
     */
    /**
   #ifdef TIME
    gettimeofday(&start_iter_h264, NULL);
   #endif
    h264_dict_entry_t* hdep = 0x0;
    if (!bypass_h264_functions) {
      hdep = iterate_h264_kernel(vehicle_state);
    }
   #ifdef TIME
    gettimeofday(&stop_iter_h264, NULL);
    iter_h264_sec  += stop_iter_h264.tv_sec  - start_iter_h264.tv_sec;
    iter_h264_usec += stop_iter_h264.tv_usec - start_iter_h264.tv_usec;
   #endif
    **/
    /* The computer vision kernel performs object recognition on the
     * next image, and returns the corresponding label. 
     * This process takes place locally (i.e. within this car).
     */
   #ifdef TIME
    gettimeofday(&start_iter_cv, NULL);
   #endif
    label_t cv_tr_label = iterate_cv_kernel(vehicle_state);
   #ifdef TIME
    gettimeofday(&stop_iter_cv, NULL);
    iter_cv_sec  += stop_iter_cv.tv_sec  - start_iter_cv.tv_sec;
    iter_cv_usec += stop_iter_cv.tv_usec - start_iter_cv.tv_usec;
   #endif

    /* The radar kernel performs distance estimation on the next radar
     * data, and returns the estimated distance to the object.
     */
   #ifdef TIME
    gettimeofday(&start_iter_rad, NULL);
   #endif
    radar_dict_entry_t* rdentry_p = iterate_rad_kernel(vehicle_state);
   #ifdef TIME
    gettimeofday(&stop_iter_rad, NULL);
    iter_rad_sec  += stop_iter_rad.tv_sec  - start_iter_rad.tv_sec;
    iter_rad_usec += stop_iter_rad.tv_usec - start_iter_rad.tv_usec;
   #endif
    distance_t rdict_dist = rdentry_p->distance;
    float * radar_inputs = rdentry_p->return_data;

    /* The Viterbi decoding kernel performs Viterbi decoding on the next
     * OFDM symbol (message), and returns the extracted message.
     * This message can come from another car (including, for example,
     * its 'pose') or from the infrastructure (like speed violation or
     * road construction warnings). For simplicity, we define a fix set
     * of message classes (e.g. car on the right, car on the left, etc.)
     */
   #ifdef TIME
    gettimeofday(&start_iter_vit, NULL);
   #endif
    vit_dict_entry_t* vdentry_p = iterate_vit_kernel(vehicle_state);
   #ifdef TIME
    gettimeofday(&stop_iter_vit, NULL);
    iter_vit_sec  += stop_iter_vit.tv_sec  - start_iter_vit.tv_sec;
    iter_vit_usec += stop_iter_vit.tv_usec - start_iter_vit.tv_usec;
   #endif

    // EXECUTE the kernels using the now known inputs
    /**
   #ifdef TIME
    gettimeofday(&start_exec_h264, NULL);
   #endif
    char* found_frame_ptr = 0x0;
    if (!bypass_h264_functions) {
      execute_h264_kernel(hdep, found_frame_ptr);
    } else {
      found_frame_ptr = (char*)0xAD065BED;
    }
   #ifdef TIME
    gettimeofday(&stop_exec_h264, NULL);
    exec_h264_sec  += stop_exec_h264.tv_sec  - start_exec_h264.tv_sec;
    exec_h264_usec += stop_exec_h264.tv_usec - start_exec_h264.tv_usec;
   #endif
    **/
   #ifdef TIME
    gettimeofday(&start_exec_cv, NULL);
   #endif
    // Request a MetadataBlock (for an CV_TASK at Critical Level)
    task_metadata_block_t* cv_mb_ptr = NULL;
    if (!no_crit_cnn_task) {
      do {
        cv_mb_ptr = get_task_metadata_block(CV_TASK, CRITICAL_TASK, cv_profile);
	usleep(get_mb_holdoff);
     } while (0); // (cv_mb_ptr == NULL);
     #ifdef TIME
      struct timeval got_time;
      gettimeofday(&got_time, NULL);
      exec_get_cv_sec  += got_time.tv_sec  - start_exec_cv.tv_sec;
      exec_get_cv_usec += got_time.tv_usec - start_exec_cv.tv_usec;
     #endif
      if (cv_mb_ptr == NULL) {
        // We ran out of metadata blocks -- PANIC!
        printf("Out of metadata blocks for CV -- PANIC Quit the run (for now)\n");
	dump_all_metadata_blocks_states();
        exit (-4);
      }
      cv_mb_ptr->atFinish = NULL; // Just to ensure it is NULL
      start_execution_of_cv_kernel(cv_mb_ptr, cv_tr_label); // Critical RADAR task    label = execute_cv_kernel(cv_tr_label);
    }
    if (!no_crit_cnn_task) {
      DEBUG(printf("CV_TASK_BLOCK: ID = %u\n", cv_mb_ptr->block_id));
    }
   #ifdef TIME
    gettimeofday(&start_exec_rad, NULL);
   #endif
    // Request a MetadataBlock (for an FFT_TASK at Critical Level)
      task_metadata_block_t* fft_mb_ptr = NULL;
      do {
        fft_mb_ptr = get_task_metadata_block(FFT_TASK, CRITICAL_TASK, fft_profile[crit_fft_samples_set]);
	usleep(get_mb_holdoff);
      } while (0); //(fft_mb_ptr == NULL);
     #ifdef TIME
      struct timeval got_time;
      gettimeofday(&got_time, NULL);
      exec_get_rad_sec  += got_time.tv_sec  - start_exec_rad.tv_sec;
      exec_get_rad_usec += got_time.tv_usec - start_exec_rad.tv_usec;
     #endif
    //printf("FFT Crit Profile: %e %e %e %e %e\n", fft_profile[crit_fft_samples_set][0], fft_profile[crit_fft_samples_set][1], fft_profile[crit_fft_samples_set][2], fft_profile[crit_fft_samples_set][3], fft_profile[crit_fft_samples_set][4]);
    if (fft_mb_ptr == NULL) {
      // We ran out of metadata blocks -- PANIC!
      printf("Out of metadata blocks for FFT -- PANIC Quit the run (for now)\n");
      dump_all_metadata_blocks_states();
      exit (-4);
    }
    fft_mb_ptr->atFinish = NULL; // Just to ensure it is NULL
    start_execution_of_rad_kernel(fft_mb_ptr, radar_log_nsamples_per_dict_set[crit_fft_samples_set], radar_inputs); // Critical RADAR task
    DEBUG(printf("FFT_TASK_BLOCK: ID = %u\n", fft_mb_ptr->block_id));
   #ifdef TIME
    gettimeofday(&start_exec_vit, NULL);
   #endif
    //NOTE Removed the num_messages stuff -- need to do this differently (separate invocations of this process per message)
    // Request a MetadataBlock (for an VITERBI_TASK at Critical Level)
    task_metadata_block_t* vit_mb_ptr = NULL;
    do {
      vit_mb_ptr = get_task_metadata_block(VITERBI_TASK, 3, vit_profile[vit_msgs_size]);
      usleep(get_mb_holdoff);
    } while (0); //(vit_mb_ptr == NULL);
   #ifdef TIME
    //struct timeval got_time;
    gettimeofday(&got_time, NULL);
    exec_get_vit_sec  += got_time.tv_sec  - start_exec_vit.tv_sec;
    exec_get_vit_usec += got_time.tv_usec - start_exec_vit.tv_usec;
   #endif
    if (vit_mb_ptr == NULL) {
      // We ran out of metadata blocks -- PANIC!
      printf("Out of metadata blocks for VITERBI -- PANIC Quit the run (for now)\n");
	dump_all_metadata_blocks_states();
      exit (-4);
    }
    vit_mb_ptr->atFinish = NULL; // Just to ensure it is NULL
    start_execution_of_vit_kernel(vit_mb_ptr, vdentry_p); // Critical VITERBI task
    DEBUG(printf("VIT_TASK_BLOCK: ID = %u\n", vit_mb_ptr->block_id));

    // Now we add in the additional non-critical tasks...
    for (int i = 0; i < max_additional_tasks_per_time_step; i++) {
      // Aditional CV Tasks
      //for (int i = 0; i < additional_cv_tasks_per_time_step; i++) {
      if (i < additional_cv_tasks_per_time_step) {
       #ifdef TIME
        struct timeval get_time;
        gettimeofday(&get_time, NULL);
       #endif
        task_metadata_block_t* cv_mb_ptr_2 = NULL;
        do {
          cv_mb_ptr_2 = get_task_metadata_block(CV_TASK, BASE_TASK, cv_profile);
	  //usleep(get_mb_holdoff);
        } while (0); //(cv_mb_ptr_2 == NULL);
       #ifdef TIME
        struct timeval got_time;
        gettimeofday(&got_time, NULL);
        exec_get_cv_sec  += got_time.tv_sec  - get_time.tv_sec;
        exec_get_cv_usec += got_time.tv_usec - get_time.tv_usec;
       #endif
        if (cv_mb_ptr_2 == NULL) {
  	  printf("Out of metadata blocks for Non-Critical CV -- PANIC Quit the run (for now)\n");
  	  dump_all_metadata_blocks_states();
	  exit (-5);
        }
        cv_mb_ptr_2->atFinish = base_release_metadata_block;
        start_execution_of_cv_kernel(cv_mb_ptr_2, cv_tr_label); // NON-Critical RADAR task
      } // if (i < additional CV tasks)
      //for (int i = 0; i < additional_fft_tasks_per_time_step; i++) {
      if (i < additional_fft_tasks_per_time_step) {
        radar_dict_entry_t* rdentry_p2;
	if (task_size_variability == 0) {
	  rdentry_p2 = select_critical_radar_input(rdentry_p);
	} else {
	  rdentry_p2 = select_random_radar_input();
	  //printf("FFT select: Crit %u rdp2->set %u\n", crit_fft_samples_set, rdentry_p2->set);
	}
	int base_fft_samples_set = rdentry_p2->set;
	//printf("FFT Base Profile: %e %e %e %e %e\n", fft_profile[base_fft_samples_set][0], fft_profile[base_fft_samples_set][1], fft_profile[base_fft_samples_set][2], fft_profile[base_fft_samples_set][3], fft_profile[base_fft_samples_set][4]);
       #ifdef TIME
	struct timeval get_time;
	gettimeofday(&get_time, NULL);
       #endif
	task_metadata_block_t* fft_mb_ptr_2 = NULL;
        do {
	  fft_mb_ptr_2 = get_task_metadata_block(FFT_TASK, BASE_TASK, fft_profile[base_fft_samples_set]);
	  //usleep(get_mb_holdoff);
        } while (0); //(fft_mb_ptr_2 == NULL);
       #ifdef TIME
        //struct timeval got_time;
        gettimeofday(&got_time, NULL);
	exec_get_rad_sec  += got_time.tv_sec  - get_time.tv_sec;
	exec_get_rad_usec += got_time.tv_usec - get_time.tv_usec;
       #endif
        if (fft_mb_ptr_2 == NULL) {
  	  printf("Out of metadata blocks for Non-Critical FFT -- PANIC Quit the run (for now)\n");
	  dump_all_metadata_blocks_states();
	  exit (-5);
        }
        fft_mb_ptr_2->atFinish = base_release_metadata_block;
	float* addl_radar_inputs = rdentry_p2->return_data;
	start_execution_of_rad_kernel(fft_mb_ptr_2, radar_log_nsamples_per_dict_set[crit_fft_samples_set], addl_radar_inputs); // NON-Critical RADAR task
      } // if (i < additional FFT tasks)

      //for (int i = 0; i < additional_vit_tasks_per_time_step; i++) {
      if (i < additional_vit_tasks_per_time_step) {
        vit_dict_entry_t* vdentry2_p;
	int base_msg_size;
        if (task_size_variability == 0) {
	  base_msg_size = vdentry_p->msg_num / NUM_MESSAGES;
	  int m_id = vdentry_p->msg_num % NUM_MESSAGES;
	  if (m_id != vdentry_p->msg_id) {
	    printf("WARNING: MSG_NUM %u : LNUM %u M_ID %u MSG_ID %u\n", vdentry_p->msg_num, base_msg_size, m_id, vdentry_p->msg_id);
          }
	  if (base_msg_size != vit_msgs_size) {
	    printf("WARNING: MSG_NUM %u : LNUM %u M_ID %u MSG_ID %u\n", vdentry_p->msg_num, base_msg_size, m_id, vdentry_p->msg_id);
	  }
	  vdentry2_p = select_specific_vit_input(base_msg_size, m_id);
        } else {
	  DEBUG(printf("Note: electing a random Vit Message for base-task\n"));
	  vdentry2_p = select_random_vit_input();
	  base_msg_size = vdentry2_p->msg_num / NUM_MESSAGES;
        }
       #ifdef TIME
        struct timeval get_time;
	gettimeofday(&get_time, NULL);
       #endif
        task_metadata_block_t* vit_mb_ptr_2 = NULL;
        do {
          vit_mb_ptr_2 = get_task_metadata_block(VITERBI_TASK, BASE_TASK, vit_profile[base_msg_size]);
	  //usleep(get_mb_holdoff);
        } while (0); // (vit_mb_ptr_2 == NULL);
       #ifdef TIME
        struct timeval got_time;
	gettimeofday(&got_time, NULL);
	exec_get_vit_sec  += got_time.tv_sec  - get_time.tv_sec;
	exec_get_vit_usec += got_time.tv_usec - get_time.tv_usec;
       #endif
        if (vit_mb_ptr_2 == NULL) {
  	  printf("Out of metadata blocks for Non-Critical VIT -- PANIC Quit the run (for now)\n");
	  dump_all_metadata_blocks_states();
	  exit (-5);
        }
        vit_mb_ptr_2->atFinish = base_release_metadata_block;
        start_execution_of_vit_kernel(vit_mb_ptr_2, vdentry2_p); // Non-Critical VITERBI task
      } // if (i < Additional VIT tasks)
    } // for (i over MAX_additional_tasks)
   #ifdef TIME
    gettimeofday(&start_wait_all_crit, NULL);
   #endif

    DEBUG(printf("MAIN: Calling wait_all_critical\n"));
    wait_all_critical();

   #ifdef TIME
    gettimeofday(&stop_wait_all_crit, NULL);
    wait_all_crit_sec  += stop_wait_all_crit.tv_sec  - start_wait_all_crit.tv_sec;
    wait_all_crit_usec += stop_wait_all_crit.tv_usec - start_wait_all_crit.tv_usec;
   #endif
    
    distance = finish_execution_of_rad_kernel(fft_mb_ptr);
    message = finish_execution_of_vit_kernel(vit_mb_ptr);
    if (!no_crit_cnn_task) {
      label = finish_execution_of_cv_kernel(cv_mb_ptr);
    }
   #ifdef TIME
    gettimeofday(&stop_exec_rad, NULL);
    exec_rad_sec  += stop_exec_rad.tv_sec  - start_exec_rad.tv_sec;
    exec_rad_usec += stop_exec_rad.tv_usec - start_exec_rad.tv_usec;
    exec_vit_sec  += stop_exec_rad.tv_sec  - start_exec_vit.tv_sec;
    exec_vit_usec += stop_exec_rad.tv_usec - start_exec_vit.tv_usec;
    exec_cv_sec   += stop_exec_rad.tv_sec  - start_exec_cv.tv_sec;
    exec_cv_usec  += stop_exec_rad.tv_usec - start_exec_cv.tv_usec;
   #endif

    // POST-EXECUTE each kernel to gather stats, etc.
    /*if (!bypass_h264_functions) {
      post_execute_h264_kernel();
      }*/
    post_execute_cv_kernel(cv_tr_label, label);
    post_execute_rad_kernel(rdentry_p->set, rdentry_p->index_in_set, rdict_dist, distance);
    post_execute_vit_kernel(vdentry_p->msg_id, message);

    /* The plan_and_control() function makes planning and control decisions
     * based on the currently perceived information. It returns the new
     * vehicle state.
     */
    DEBUG(printf("Time Step %3u : Calling Plan and Control %u times with message %u and distance %.1f\n", time_step, pandc_repeat_factor, time_step, message, distance));
    vehicle_state_t new_vehicle_state;
   #ifdef TIME
    gettimeofday(&start_exec_pandc, NULL);
   #endif
    for (int prfi = 0; prfi <= pandc_repeat_factor; prfi++) {
      new_vehicle_state = plan_and_control(label, distance, message, vehicle_state);
    }
   #ifdef TIME
    gettimeofday(&stop_exec_pandc, NULL);
    exec_pandc_sec  += stop_exec_pandc.tv_sec  - start_exec_pandc.tv_sec;
    exec_pandc_usec += stop_exec_pandc.tv_usec - start_exec_pandc.tv_usec;
   #endif
    vehicle_state = new_vehicle_state;

    DEBUG(printf("New vehicle state: lane %u speed %.1f\n\n", vehicle_state.lane, vehicle_state.speed));

    time_step++;

    // TEST - trying this here.
    //wait_all_tasks_finish();
    
    #ifndef USE_SIM_ENVIRON
    read_next_trace_record(vehicle_state);
    #endif
  }

  // This is the end of time steps... wait for all tasks to be finished (?)
  // Adding this results in never completing...  not sure why.
  // wait_all_tasks_finish();
  
 #ifdef TIME
  gettimeofday(&stop_prog, NULL);
 #endif

  /* All the trace/simulation-time has been completed -- Quitting... */
  printf("\nRun completed %u time steps\n\n", time_step);

  if (!bypass_h264_functions) {
    //closeout_h264_kernel();
  }
  closeout_cv_kernel();
  closeout_rad_kernel();
  closeout_vit_kernel();

  #ifdef TIME
  {
    uint64_t total_exec = (uint64_t) (stop_prog.tv_sec - start_prog.tv_sec) * 1000000 + (uint64_t) (stop_prog.tv_usec - start_prog.tv_usec);
    uint64_t iter_rad   = (uint64_t) (iter_rad_sec) * 1000000 + (uint64_t) (iter_rad_usec);
    uint64_t iter_vit   = (uint64_t) (iter_vit_sec) * 1000000 + (uint64_t) (iter_vit_usec);
    uint64_t iter_cv    = (uint64_t) (iter_cv_sec)  * 1000000 + (uint64_t) (iter_cv_usec);
    //uint64_t iter_h264  = (uint64_t) (iter_h264_sec) * 1000000 + (uint64_t) (iter_h264_usec);
    uint64_t exec_rad   = (uint64_t) (exec_rad_sec) * 1000000 + (uint64_t) (exec_rad_usec);
    uint64_t exec_vit   = (uint64_t) (exec_vit_sec) * 1000000 + (uint64_t) (exec_vit_usec);
    uint64_t exec_cv    = (uint64_t) (exec_cv_sec)  * 1000000 + (uint64_t) (exec_cv_usec);
    //uint64_t exec_h264  = (uint64_t) (exec_h264_sec) * 1000000 + (uint64_t) (exec_h264_usec);
    uint64_t exec_get_rad   = (uint64_t) (exec_get_rad_sec) * 1000000 + (uint64_t) (exec_get_rad_usec);
    uint64_t exec_get_vit   = (uint64_t) (exec_get_vit_sec) * 1000000 + (uint64_t) (exec_get_vit_usec);
    uint64_t exec_get_cv    = (uint64_t) (exec_get_cv_sec)  * 1000000 + (uint64_t) (exec_get_cv_usec);
    //uint64_t exec_get_h264  = (uint64_t) (exec_h264_sec) * 1000000 + (uint64_t) (exec_h264_usec);
    uint64_t exec_pandc = (uint64_t) (exec_pandc_sec) * 1000000 + (uint64_t) (exec_pandc_usec);
    uint64_t wait_all_crit   = (uint64_t) (wait_all_crit_sec) * 1000000 + (uint64_t) (wait_all_crit_usec);
    printf("\nProgram total execution time      %lu usec\n", total_exec);
    printf("  iterate_rad_kernel run time       %lu usec\n", iter_rad);
    printf("  iterate_vit_kernel run time       %lu usec\n", iter_vit);
    printf("  iterate_cv_kernel run time        %lu usec\n", iter_cv);
    //printf("  iterate_h264_kernel run time      %lu usec\n", iter_h264);
    printf("  Crit execute_rad_kernel run time  %lu usec\n", exec_rad);
    printf("  Crit execute_vit_kernel run time  %lu usec\n", exec_vit);
    printf("  Crit execute_cv_kernel run time   %lu usec\n", exec_cv);
    printf("    GET_MB execute_rad_kernel run time  %lu usec\n", exec_rad);
    printf("    GET_MB execute_vit_kernel run time  %lu usec\n", exec_vit);
    printf("    GET_MB execute_cv_kernel run time   %lu usec\n", exec_cv);
    //printf("  execute_h264_kernel run time      %lu usec\n", exec_h264);
    printf("  plan_and_control run time         %lu usec at %u factor\n", exec_pandc, pandc_repeat_factor);
    printf("  wait_all_critical run time        %lu usec\n", wait_all_crit);
  }
 #endif // TIME
  shutdown_scheduler();
  printf("\nDone.\n");
  return 0;
}
