#include <data_types/timeseries.hpp>
#include <data_types/fourierseries.hpp>
#include <data_types/candidates.hpp>
#include <data_types/filterbank.hpp>
#include <transforms/dedisperser.hpp>
#include <transforms/resampler.hpp>
#include <transforms/folder.hpp>
#include <transforms/ffter.hpp>
#include <transforms/dereddener.hpp>
#include <transforms/spectrumformer.hpp>
#include <transforms/birdiezapper.hpp>
#include <transforms/peakfinder.hpp>
#include <transforms/distiller.hpp>
#include <transforms/harmonicfolder.hpp>
#include <transforms/scorer.hpp>
#include <utils/exceptions.hpp>
#include <utils/utils.hpp>
#include <utils/stats.hpp>
#include <utils/stopwatch.hpp>
#include <utils/progress_bar.hpp>
#include <tclap/CmdLine.h>
#include <string>
#include <iostream>
#include <stdio.h>
#include <unistd.h>
#include "cuda.h"
#include "cufft.h"
#include "pthread.h"
#include <cmath>

struct CmdLineOptions {
  std::string infilename;
  std::string output_directory;
  std::string killfilename;
  std::string zapfilename;
  int max_num_threads;
  unsigned int size; 
  float dm_start; 
  float dm_end;
  float dm_tol;
  float dm_pulse_width;
  float acc_start;
  float acc_end;
  float acc_tol;
  float acc_pulse_width;
  float boundary_5_freq;
  float boundary_25_freq;
  int nharmonics;
  float min_snr;
  float min_freq;
  float max_freq;
  int max_harm;
  float freq_tol;
  bool verbose;
  bool progress_bar;
};

class DMDispenser {
private:
  DispersionTrials<unsigned char>& trials;
  pthread_mutex_t mutex;
  int dm_idx;
  int count;
  ProgressBar* progress;
  bool use_progress_bar;

public:
  DMDispenser(DispersionTrials<unsigned char>& trials)
    :trials(trials),dm_idx(0),use_progress_bar(false){
    count = trials.get_count();
    pthread_mutex_init(&mutex, NULL);
  }
  
  void enable_progress_bar(){
    progress = new ProgressBar();
    use_progress_bar = true;
  }

  int get_dm_trial_idx(void){
    pthread_mutex_lock(&mutex);
    int retval;
    if (dm_idx==0)
      if (use_progress_bar){
	printf("Releasing DMs to workers...\n");
	progress->start();
      }
    if (dm_idx >= trials.get_count()){
      retval =  -1;
      if (use_progress_bar)
	progress->stop();
    } else {
      if (use_progress_bar)
	progress->set_progress((float)dm_idx/count);
      retval = dm_idx;
      dm_idx++;
    }
    pthread_mutex_unlock(&mutex);
    return retval;
  }
  
  ~DMDispenser(){
    if (use_progress_bar)
      delete progress;
    pthread_mutex_destroy(&mutex);
  }
};

class Worker {
private:
  DispersionTrials<unsigned char>& trials;
  DMDispenser& manager;
  CmdLineOptions& args;
  AccelerationPlan& acc_plan;
  unsigned int size;
  int device;
  
public:
  CandidateCollection dm_trial_cands;

  Worker(DispersionTrials<unsigned char>& trials, DMDispenser& manager, 
	 AccelerationPlan& acc_plan, CmdLineOptions& args, unsigned int size, int device)
    :trials(trials),manager(manager),acc_plan(acc_plan),args(args),size(size),device(device){}
  
  void start(void)
  {
    cudaSetDevice(device);

    bool padding = false;
    if (size > trials.get_nsamps())
      padding = true;
    
    CuFFTerR2C r2cfft(size);
    CuFFTerC2R c2rfft(size);
    float tobs = size*trials.get_tsamp();
    float bin_width = 1.0/tobs;
    DeviceFourierSeries<cufftComplex> d_fseries(size/2+1,bin_width);
    DedispersedTimeSeries<unsigned char> tim;
    ReusableDeviceTimeSeries<float,unsigned char> d_tim(size);
    DeviceTimeSeries<float> d_tim_r(size);
    TimeDomainResampler resampler;
    DevicePowerSpectrum<float> pspec(d_fseries);
    Zapper* bzap;
    if (args.zapfilename!=""){
      if (args.verbose)
	std::cout << "Using zapfile: " << args.zapfilename << std::endl;
      bzap = new Zapper(args.zapfilename);
    }
    Dereddener rednoise(size/2+1);
    SpectrumFormer former;
    PeakFinder cand_finder(args.min_snr,args.min_freq,args.max_freq);
    HarmonicFolder harm_folder;
    HarmonicSums<float> sums(pspec,args.nharmonics);
    std::vector<float> acc_list;
    HarmonicDistiller harm_finder(args.freq_tol,args.max_harm,false);
    AccelerationDistiller acc_still(tobs,args.freq_tol,true);
    float mean,std,rms;
    float padding_mean;
    int ii;

    while (true){
      ii = manager.get_dm_trial_idx();
      if (ii==-1)
	break;
      trials.get_idx(ii,tim);
      
      if (args.verbose)
	std::cout << "Copying DM trial to device (DM: " << tim.get_dm() << ")"<< std::endl;
      d_tim.copy_from_host(tim);
      
      if (padding){
	padding_mean = stats::mean<float>(d_tim.get_data(),trials.get_nsamps());
	d_tim.fill(trials.get_nsamps(),d_tim.get_nsamps(),padding_mean);
      }

      if (args.verbose)
	std::cout << "Generating accelration list" << std::endl;
      acc_plan.generate_accel_list(tim.get_dm(),acc_list);
      
      if (args.verbose)
	std::cout << "Executing forward FFT" << std::endl;
      r2cfft.execute(d_tim.get_data(),d_fseries.get_data());

      if (args.verbose)
	std::cout << "Forming power spectrum" << std::endl;
      former.form(d_fseries,pspec);

      if (args.verbose)
	std::cout << "Finding running median" << std::endl;
      rednoise.calculate_median(pspec);

      if (args.verbose)
	std::cout << "Dereddening Fourier series" << std::endl;
      rednoise.deredden(d_fseries);

      if (args.zapfilename!=""){
	if (args.verbose)
	  std::cout << "Zapping birdies" << std::endl;
	bzap->zap(d_fseries);
      }

      if (args.verbose)
	std::cout << "Forming interpolated power spectrum" << std::endl;
      former.form_interpolated(d_fseries,pspec);

      if (args.verbose)
	std::cout << "Finding statistics" << std::endl;
      stats::stats<float>(pspec.get_data(),size/2+1,&mean,&rms,&std);

      if (args.verbose)
	std::cout << "Executing inverse FFT" << std::endl;
      c2rfft.execute(d_fseries.get_data(),d_tim.get_data());
      
      CandidateCollection accel_trial_cands;    
      for (int jj=0;jj<acc_list.size();jj++){
	if (args.verbose)
	  std::cout << "Resampling to "<< acc_list[jj] << " m/s/s" << std::endl;
	resampler.resample(d_tim,d_tim_r,size,acc_list[jj]);

	
	if (args.verbose)
	  std::cout << "Execute forward FFT" << std::endl;
	r2cfft.execute(d_tim_r.get_data(),d_fseries.get_data());

	if (args.verbose)
	  std::cout << "Form interpolated power spectrum" << std::endl;
	former.form_interpolated(d_fseries,pspec);

	if (args.verbose)
	  std::cout << "Normalise power spectrum" << std::endl;
	stats::normalise(pspec.get_data(),mean*size,std*size,size/2+1);

	if (args.verbose)
	  std::cout << "Harmonic summing" << std::endl;
	harm_folder.fold(pspec,sums);

	if (args.verbose)
	  std::cout << "Finding peaks" << std::endl;
	SpectrumCandidates trial_cands(tim.get_dm(),ii,acc_list[jj]);
	cand_finder.find_candidates(pspec,trial_cands);
	cand_finder.find_candidates(sums,trial_cands);
	
	if (args.verbose)
	  std::cout << "Distilling harmonics" << std::endl;
	accel_trial_cands.append(harm_finder.distill(trial_cands.cands));
      }
      if (args.verbose)
	std::cout << "Distilling accelerations" << std::endl;
      dm_trial_cands.append(acc_still.distill(accel_trial_cands.cands));
    }

    if (args.zapfilename!="")
      delete bzap;
  }
  
};

void* launch_worker_thread(void* ptr){
  reinterpret_cast<Worker*>(ptr)->start();
  return NULL;
}


int main(int argc, char **argv)
{
  CmdLineOptions args;
  try
    {
      TCLAP::CmdLine cmd("Peasoup - a GPU pulsar search pipeline", ' ', "1.0");
      
      TCLAP::ValueArg<std::string> arg_infilename("i", "inputfile",
						  "File to process (.fil)",
						  true, "", "string", cmd);
      
      TCLAP::ValueArg<std::string> arg_output_directory("o", "outputfile",
							"The output filename",
							false, "./", "string",cmd);
      
      TCLAP::ValueArg<std::string> arg_killfilename("k", "killfile",
                                                   "Channel mask file",
                                                   false, "", "string",cmd);
      
      TCLAP::ValueArg<std::string> arg_zapfilename("z", "zapfile",
						   "Birdie list file",
						   false, "", "string",cmd);

      TCLAP::ValueArg<int> arg_max_num_threads("t", "num_threads", 
					       "The number of GPUs to use",
					       false, 14, "int", cmd);
      
      TCLAP::ValueArg<size_t> arg_size("", "fft_size",
				       "Transform size to use (defaults to lower power of two)",
				       false, 0, "size_t", cmd);
      
      TCLAP::ValueArg<float> arg_dm_start("", "dm_start", 
					  "First DM to dedisperse to",
					  false, 0.0, "float", cmd);
      
      TCLAP::ValueArg<float> arg_dm_end("", "dm_end",
					"Last DM to dedisperse to",
					false, 100.0, "float", cmd);
      
      TCLAP::ValueArg<float> arg_dm_tol("", "dm_tol",
					"DM smearing tolerance (1.11=10%)",
					false, 1.10, "float",cmd);

      TCLAP::ValueArg<float> arg_dm_pulse_width("", "dm_pulse_width",
						"Minimum pulse width for which dm_tol is valid",
						false, 64.0, "float (us)",cmd);

      TCLAP::ValueArg<float> arg_acc_start("", "acc_start",
                                          "First acceleration to resample to",
                                          false, 0.0, "float", cmd);

      TCLAP::ValueArg<float> arg_acc_end("", "acc_end",
                                        "Last acceleration to resample to",
                                        false, 0.0, "float", cmd);

      TCLAP::ValueArg<float> arg_acc_tol("", "acc_tol",
                                        "Acceleration smearing tolerance (1.11=10%)",
                                        false, 1.10, "float",cmd);

      TCLAP::ValueArg<float> arg_acc_pulse_width("", "acc_pulse_width",
						 "Minimum pulse width for which acc_tol is valid",
                                                false, 64.0, "float (ms)",cmd);
            
      TCLAP::ValueArg<float> arg_boundary_5_freq("", "boundary_5_freq",
						 "Frequency at which to switch from median5 to median25",
						 false, 0.05, "float", cmd);
      
      TCLAP::ValueArg<float> arg_boundary_25_freq("", "boundary_25_freq",
						 "Frequency at which to switch from median25 to median125",
                                                 false, 0.5, "float", cmd);
      
      TCLAP::ValueArg<int> arg_nharmonics("n", "nharmonics",
					  "Number of harmonic sums to perform",
					  false, 4, "int", cmd);

      TCLAP::ValueArg<float> arg_min_snr("m", "min_snr", 
					 "The minimum S/N for a candidate",
					 false, 9.0, "float",cmd);
      
      TCLAP::ValueArg<float> arg_min_freq("", "min_freq",
					  "Lowest Fourier freqency to consider",
					  false, 0.1, "float",cmd);
      
      TCLAP::ValueArg<float> arg_max_freq("", "max_freq",
                                          "Highest Fourier freqency to consider",
                                          false, 1100.0, "float",cmd);

      TCLAP::ValueArg<int> arg_max_harm("", "max_harm_match",
					"Maximum harmonic for related candidates",
					false, 16, "float",cmd);
      
      TCLAP::ValueArg<float> arg_freq_tol("", "freq_tol",
                                          "Tolerance for distilling frequencies (0.0001 = 0.01%)",
                                          false, 0.0001, "float",cmd);
      
      TCLAP::SwitchArg arg_verbose("v", "verbose", "verbose mode", cmd);
      
      TCLAP::SwitchArg arg_progress_bar("p", "progress_bar", "Enable progress bar for DM search", cmd);

      cmd.parse(argc, argv);
      args.infilename        = arg_infilename.getValue();
      args.output_directory  = arg_output_directory.getValue();
      args.killfilename      = arg_killfilename.getValue();
      args.zapfilename       = arg_zapfilename.getValue();
      args.max_num_threads   = arg_max_num_threads.getValue();
      args.size              = arg_size.getValue();
      args.dm_start          = arg_dm_start.getValue();
      args.dm_end            = arg_dm_end.getValue();
      args.dm_tol            = arg_dm_tol.getValue();
      args.dm_pulse_width    = arg_dm_pulse_width.getValue();
      args.acc_start         = arg_acc_start.getValue();
      args.acc_end           = arg_acc_end.getValue();
      args.acc_tol           = arg_acc_tol.getValue();
      args.acc_pulse_width   = arg_acc_pulse_width.getValue();
      args.boundary_5_freq   = arg_boundary_5_freq.getValue();   
      args.boundary_25_freq  = arg_boundary_25_freq.getValue();
      args.nharmonics        = arg_nharmonics.getValue();
      args.min_snr           = arg_min_snr.getValue();
      args.min_freq          = arg_min_freq.getValue();
      args.max_freq          = arg_max_freq.getValue();
      args.max_harm          = arg_max_harm.getValue();
      args.freq_tol          = arg_freq_tol.getValue();
      args.verbose           = arg_verbose.getValue();
      args.progress_bar      = arg_progress_bar.getValue();
      
    }catch (TCLAP::ArgException &e) {
    std::cerr << "Error: " << e.error() << " for arg " << e.argId()
	      << std::endl;
    return -1;
  }

  int nthreads = std::min(Utils::gpu_count(),args.max_num_threads);
  nthreads = std::max(1,nthreads);

  if (args.verbose)
    std::cout << "Using file: " << args.infilename << std::endl;
  std::string filename(args.infilename);

  Stopwatch timer;
  if (args.progress_bar){
    printf("Reading data from %s\n",args.infilename.c_str());
    timer.start();
  }
  SigprocFilterbank filobj(filename);
  if (args.progress_bar){
    timer.stop();
    printf("Complete (execution time %.2f s)\n",timer.getTime());
  }

  Dedisperser dedisperser(filobj,nthreads);
  if (args.killfilename!=""){
    if (args.verbose)
      std::cout << "Using killfile: " << args.killfilename << std::endl;
    dedisperser.set_killmask(args.killfilename);
  }
  
  if (args.verbose)
    std::cout << "Generating DM list" << std::endl;
  dedisperser.generate_dm_list(args.dm_start,args.dm_end,args.dm_pulse_width,args.dm_tol);
  
  if (args.verbose){
    std::vector<float> dm_list = dedisperser.get_dm_list();
    std::cout << dm_list.size() << " DM trials" << std::endl;
    for (int ii=0;ii<dm_list.size();ii++)
      std::cout << dm_list[ii] << std::endl;
    std::cout << "Executing dedispersion" << std::endl;
  }

  if (args.progress_bar){
    printf("Starting dedispersion...\n");
    timer.start();
  }
  DispersionTrials<unsigned char> trials = dedisperser.dedisperse();
  if (args.progress_bar){
    timer.stop();
    printf("Complete (execution time %.2f s)\n",timer.getTime());
  }

  unsigned int size;
  if (args.size==0)
    size = Utils::prev_power_of_two(filobj.get_nsamps());
  else
    //size = std::min(args.size,filobj.get_nsamps());
    size = args.size;
  if (args.verbose)
    std::cout << "Setting transform length to " << size << " points" << std::endl;
  
  AccelerationPlan acc_plan(args.acc_start, args.acc_end, args.acc_tol,
			    args.acc_pulse_width, size, filobj.get_tsamp(),
			    filobj.get_cfreq(), filobj.get_foff()); 
  
  //Multithreading commands
  
  std::vector<Worker*> workers(nthreads);
  std::vector<pthread_t> threads(nthreads);
  DMDispenser dispenser(trials);
  if (args.progress_bar)
    dispenser.enable_progress_bar();
  
  for (int ii=0;ii<nthreads;ii++){
    workers[ii] = (new Worker(trials,dispenser,acc_plan,args,size,ii));
    pthread_create(&threads[ii], NULL, launch_worker_thread, (void*) workers[ii]);
  }
  
  DMDistiller dm_still(args.freq_tol,true);
  HarmonicDistiller harm_still(args.freq_tol,args.max_harm,true,false);
  CandidateCollection dm_cands;
  for (int ii=0; ii<nthreads; ii++){
    pthread_join(threads[ii],NULL);
    dm_cands.append(workers[ii]->dm_trial_cands.cands);
  }
  
  //dm_cands.print();
  if (args.verbose)
    std::cout << "Distilling DMs" << std::endl;
  dm_cands.cands = dm_still.distill(dm_cands.cands);
  //dm_cands.print();
  dm_cands.cands = harm_still.distill(dm_cands.cands);
  
  CandidateScorer cand_scorer(filobj.get_tsamp(),filobj.get_cfreq(), filobj.get_foff(),
			      fabs(filobj.get_foff())*filobj.get_nchans());
  cand_scorer.score_all(dm_cands.cands);

  if (args.verbose)
    std::cout << "Setting up time series folder" << std::endl;
  
    
  MultiFolder folder(dm_cands.cands,trials);
  if (args.progress_bar)
    folder.enable_progress_bar();

  if (args.verbose)
    std::cout << "Folding top 3000 cands" << std::endl;
  folder.fold_n(3000);

  if (args.verbose)
    std::cout << "Writing output files" << std::endl;
  dm_cands.write_candidate_file(args.output_directory);
  
  return 0;
}
