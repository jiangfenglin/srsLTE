/**
 *
 * \section COPYRIGHT
 *
 * Copyright 2013-2015 Software Radio Systems Limited
 *
 * \section LICENSE
 *
 * This file is part of the srsLTE library.
 *
 * srsLTE is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as
 * published by the Free Software Foundation, either version 3 of
 * the License, or (at your option) any later version.
 *
 * srsLTE is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * A copy of the GNU Affero General Public License can be found in
 * the LICENSE file in the top-level directory of this distribution
 * and at http://www.gnu.org/licenses/.
 *
 */

#include <uhd.h>
#include <sys/time.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>

#include "srslte/srslte.h"
#include "rf_uhd_imp.h"
#include "srslte/phy/rf/rf.h"
#include "uhd_c_api.h"

typedef struct {
  char *devname; 
  uhd_usrp_handle usrp;
  uhd_rx_streamer_handle rx_stream;
  uhd_tx_streamer_handle tx_stream;
  
  uhd_rx_metadata_handle rx_md, rx_md_first; 
  uhd_tx_metadata_handle tx_md; 
  
  uhd_meta_range_handle rx_gain_range;
  size_t rx_nof_samples;
  size_t tx_nof_samples;
  double tx_rate;
  bool dynamic_rate; 
  bool has_rssi; 
  uhd_sensor_value_handle rssi_value;
  uint32_t nof_rx_channels;
  int nof_tx_channels;

  srslte_rf_error_handler_t uhd_error_handler; 
  
  bool async_thread_running; 
  pthread_t async_thread; 
} rf_uhd_handler_t;

void suppress_handler(const char *x)
{
  // do nothing
}

cf_t zero_mem[64*1024];

static void log_overflow(rf_uhd_handler_t *h) {  
  if (h->uhd_error_handler) {
    srslte_rf_error_t error; 
    bzero(&error, sizeof(srslte_rf_error_t));
    error.type = SRSLTE_RF_ERROR_OVERFLOW;
    h->uhd_error_handler(error);
  }
}

static void log_late(rf_uhd_handler_t *h) {  
  if (h->uhd_error_handler) {
    srslte_rf_error_t error; 
    bzero(&error, sizeof(srslte_rf_error_t));
    error.type = SRSLTE_RF_ERROR_LATE;
    h->uhd_error_handler(error);
  }
}

static void log_underflow(rf_uhd_handler_t *h) {  
  if (h->uhd_error_handler) {
    srslte_rf_error_t error; 
    bzero(&error, sizeof(srslte_rf_error_t));
    error.type = SRSLTE_RF_ERROR_UNDERFLOW;
    h->uhd_error_handler(error);
  }
}

static void* async_thread(void *h) {
  rf_uhd_handler_t *handler = (rf_uhd_handler_t*) h; 
  uhd_async_metadata_handle md; 
  uhd_async_metadata_make(&md); 
  while(handler->async_thread_running) {
    bool valid; 
    uhd_error err = uhd_tx_streamer_recv_async_msg(handler->tx_stream, &md, 0.5, &valid);
    if (err == UHD_ERROR_NONE) {
      if (valid) {
        uhd_async_metadata_event_code_t event_code; 
        uhd_async_metadata_event_code(md, &event_code);
        if (event_code == UHD_ASYNC_METADATA_EVENT_CODE_UNDERFLOW || 
            event_code == UHD_ASYNC_METADATA_EVENT_CODE_UNDERFLOW_IN_PACKET) {
          log_underflow(handler);
        } else if (event_code == UHD_ASYNC_METADATA_EVENT_CODE_TIME_ERROR) {
          log_late(handler);
        }
      }
    } else {
      fprintf(stderr, "Error while receiving aync metadata: 0x%x\n", err);
      return NULL; 
    }
  }
  return NULL; 
}

void rf_uhd_suppress_stdout(void *h) {
  rf_uhd_register_msg_handler_c(suppress_handler);
}

void rf_uhd_register_error_handler(void *h, srslte_rf_error_handler_t new_handler)
{
  rf_uhd_handler_t *handler = (rf_uhd_handler_t*) h;
  handler->uhd_error_handler = new_handler;
}

static bool find_string(uhd_string_vector_handle h, char *str) 
{
  char buff[128];
  size_t n;
  uhd_string_vector_size(h, &n);
  for (int i=0;i<n;i++) {
    uhd_string_vector_at(h, i, buff, 128);
    if (strstr(buff, str)) {
      return true; 
    }
  }
  return false; 
}

static bool isLocked(rf_uhd_handler_t *handler, char *sensor_name, bool is_rx, uhd_sensor_value_handle *value_h)
{
  bool val_out = false; 
  
  if (sensor_name) {
    if (is_rx) {
      uhd_usrp_get_rx_sensor(handler->usrp, sensor_name, 0, value_h);
    } else {
      uhd_usrp_get_mboard_sensor(handler->usrp, sensor_name, 0, value_h);
    }
    uhd_sensor_value_to_bool(*value_h, &val_out);
  } else {
    usleep(500);
    val_out = true; 
  }
    
  return val_out;
}

char* rf_uhd_devname(void* h)
{
  rf_uhd_handler_t *handler = (rf_uhd_handler_t*) h;
  return handler->devname; 
}

bool rf_uhd_rx_wait_lo_locked(void *h)
{
  rf_uhd_handler_t *handler = (rf_uhd_handler_t*) h;
  
  uhd_string_vector_handle mb_sensors;
  uhd_string_vector_handle rx_sensors;
  char *sensor_name;
  uhd_sensor_value_handle value_h;
  uhd_string_vector_make(&mb_sensors);
  uhd_string_vector_make(&rx_sensors);
  uhd_sensor_value_make_from_bool(&value_h, "", true, "True", "False");
  uhd_usrp_get_mboard_sensor_names(handler->usrp, 0, &mb_sensors);
  uhd_usrp_get_rx_sensor_names(handler->usrp, 0, &rx_sensors);

  /*if (find_string(rx_sensors, "lo_locked")) {
    sensor_name = "lo_locked";
  } else */if (find_string(mb_sensors, "ref_locked")) {
    sensor_name = "ref_locked";
  } else {
    sensor_name = NULL;
  }
  
  double report = 0.0;
  while (!isLocked(handler, sensor_name, false, &value_h) && report < 30.0) {
    report += 0.1;
    usleep(1000);
  }

  bool val = isLocked(handler, sensor_name, false, &value_h);
  
  uhd_string_vector_free(&mb_sensors);
  uhd_string_vector_free(&rx_sensors);
  uhd_sensor_value_free(&value_h);

  return val;
}

void rf_uhd_set_tx_cal(void *h, srslte_rf_cal_t *cal)
{
  
}

void rf_uhd_set_rx_cal(void *h, srslte_rf_cal_t *cal) 
{
  
}


int rf_uhd_start_rx_stream(void *h)
{
 rf_uhd_handler_t *handler = (rf_uhd_handler_t*) h;

  uhd_stream_cmd_t stream_cmd = {
        .stream_mode = UHD_STREAM_MODE_START_CONTINUOUS,
        .stream_now = false
  };
  uhd_usrp_get_time_now(handler->usrp, 0, &stream_cmd.time_spec_full_secs, &stream_cmd.time_spec_frac_secs);
  stream_cmd.time_spec_frac_secs += 0.5; 
  if (stream_cmd.time_spec_frac_secs > 1) {
    stream_cmd.time_spec_frac_secs -= 1;
    stream_cmd.time_spec_full_secs += 1; 
  }
  uhd_rx_streamer_issue_stream_cmd(handler->rx_stream, &stream_cmd);  
  return 0;
}

int rf_uhd_stop_rx_stream(void *h)
{
  rf_uhd_handler_t *handler = (rf_uhd_handler_t*) h;
  uhd_stream_cmd_t stream_cmd = {
        .stream_mode = UHD_STREAM_MODE_STOP_CONTINUOUS,
        .stream_now = true
  };  
  uhd_rx_streamer_issue_stream_cmd(handler->rx_stream, &stream_cmd);
  return 0;
}

void rf_uhd_flush_buffer(void *h)
{
  int n; 
  cf_t tmp1[1024];
  cf_t tmp2[1024];
  void *data[2] = {tmp1, tmp2};
  do {
    n = rf_uhd_recv_with_time_multi(h, data, 1024, 0, NULL, NULL);
  } while (n > 0);  
}

bool rf_uhd_has_rssi(void *h) {
  rf_uhd_handler_t *handler = (rf_uhd_handler_t*) h;  
  return handler->has_rssi;
}

bool get_has_rssi(void *h) {
  rf_uhd_handler_t *handler = (rf_uhd_handler_t*) h;  
  uhd_string_vector_handle rx_sensors;  
  uhd_string_vector_make(&rx_sensors);
  uhd_usrp_get_rx_sensor_names(handler->usrp, 0, &rx_sensors);
  bool ret = find_string(rx_sensors, "rssi"); 
  uhd_string_vector_free(&rx_sensors);
  return ret; 
}

float rf_uhd_get_rssi(void *h) {
  rf_uhd_handler_t *handler = (rf_uhd_handler_t*) h;  
  if (handler->has_rssi) {
    double val_out; 
    uhd_usrp_get_rx_sensor(handler->usrp, "rssi", 0, &handler->rssi_value);
    uhd_sensor_value_to_realnum(handler->rssi_value, &val_out);
    return val_out; 
  } else {
    return 0.0;
  }
}

int rf_uhd_open(char *args, void **h)
{
  return rf_uhd_open_multi(args, h, 1);
}

int rf_uhd_open_multi(char *args, void **h, uint32_t nof_rx_antennas)
{
  if (h) {
    *h = NULL; 
    
    rf_uhd_handler_t *handler = (rf_uhd_handler_t*) malloc(sizeof(rf_uhd_handler_t));
    if (!handler) {
      perror("malloc");
      return -1; 
    }
    *h = handler; 
    
    /* Set priority to UHD threads */
    uhd_set_thread_priority(uhd_default_thread_priority, true);
    
    /* Find available devices */
    uhd_string_vector_handle devices_str;
    uhd_string_vector_make(&devices_str);
    uhd_usrp_find("", &devices_str);
    
    char args2[512]; 
    
    handler->dynamic_rate = true;
    
    // Allow NULL parameter
    if (args == NULL) {
      args = "";
    }           
    handler->devname = NULL;

    // Initialize handler
    handler->uhd_error_handler = NULL;
    
    bzero(zero_mem, sizeof(cf_t)*64*1024);
    
    /* If device type or name not given in args, choose a B200 */
    if (args[0]=='\0') {
      if (find_string(devices_str, "type=b200") && !strstr(args, "recv_frame_size")) {
        // If B200 is available, use it
        args = "type=b200,master_clock_rate=30.72e6";
        handler->devname = DEVNAME_B200;
      } else if (find_string(devices_str, "type=x300")) {
        // Else if X300 is available, set master clock rate now (can't be changed later)
        args = "type=x300,master_clock_rate=184.32e6";
        handler->dynamic_rate = false; 
        handler->devname = DEVNAME_X300;
      }
    } else {
      // If args is set and x300 type is specified, make sure master_clock_rate is defined
      if (strstr(args, "type=x300") && !strstr(args, "master_clock_rate")) {
        sprintf(args2, "%s,master_clock_rate=184.32e6",args);
        args = args2;          
        handler->dynamic_rate = false; 
        handler->devname = DEVNAME_X300;
      } else if (strstr(args, "type=b200")) {
        snprintf(args2, sizeof(args2), "%s,master_clock_rate=30.72e6", args);
        args = args2;
        handler->devname = DEVNAME_B200;
      }
    }        
    
    uhd_string_vector_free(&devices_str);
    
    /* Create UHD handler */
    if (strstr(args, "silent")) {
      rf_uhd_suppress_stdout(NULL);
    } else {
      printf("Opening USRP with args: %s\n", args);
    }
    uhd_error error = uhd_usrp_make(&handler->usrp, args);
    if (error) {
      fprintf(stderr, "Error opening UHD: code %d\n", error);
      return -1; 
    }
    
    if (!handler->devname) {
      char dev_str[1024];
      uhd_usrp_get_mboard_name(handler->usrp, 0, dev_str, 1024);
      if (strstr(dev_str, "B2") || strstr(dev_str, "B2")) {
        handler->devname = DEVNAME_B200;
      } else if (strstr(dev_str, "X3") || strstr(dev_str, "X3")) {
        handler->devname = DEVNAME_X300;        
      }
    }
    if (!handler->devname) {
      handler->devname = "uhd_unknown"; 
    }
    
    // Set external clock reference   
    if (strstr(args, "clock=external")) {
      uhd_usrp_set_clock_source(handler->usrp, "external", 0);       
    } else if (strstr(args, "clock=gpsdo")) {
      printf("Using GPSDO clock\n");
      uhd_usrp_set_clock_source(handler->usrp, "gpsdo", 0);       
    }

      
    handler->has_rssi = get_has_rssi(handler);  
    if (handler->has_rssi) {        
      uhd_sensor_value_make_from_realnum(&handler->rssi_value, "rssi", 0, "dBm", "%f");      
    }
    
    size_t channel[4] = {0, 1, 2, 3};
    uhd_stream_args_t stream_args = {
          .cpu_format = "fc32",
          .otw_format = "sc16",
          .args = "",
          .channel_list = channel,
          .n_channels = 1
      };
      
    handler->nof_rx_channels = nof_rx_antennas; 
    handler->nof_tx_channels = 1;

    /* Set default rate to avoid decimation warnings */
    uhd_usrp_set_rx_rate(handler->usrp, 1.92e6, 0);
    uhd_usrp_set_tx_rate(handler->usrp, 1.92e6, 0);
    
    /* Initialize rx and tx stremers */
    uhd_rx_streamer_make(&handler->rx_stream);
    error = uhd_usrp_get_rx_stream(handler->usrp, &stream_args, handler->rx_stream);
    if (error) {
      fprintf(stderr, "Error opening RX stream: %d\n", error);
      return -1; 
    }
    uhd_tx_streamer_make(&handler->tx_stream);
    error = uhd_usrp_get_tx_stream(handler->usrp, &stream_args, handler->tx_stream);
    if (error) {
      fprintf(stderr, "Error opening TX stream: %d\n", error);
      return -1; 
    }
    
    uhd_rx_streamer_max_num_samps(handler->rx_stream, &handler->rx_nof_samples);
    uhd_tx_streamer_max_num_samps(handler->tx_stream, &handler->tx_nof_samples);
    
    uhd_meta_range_make(&handler->rx_gain_range); 
    uhd_usrp_get_rx_gain_range(handler->usrp, "", 0, handler->rx_gain_range);

    // Make metadata objects for RX/TX
    uhd_rx_metadata_make(&handler->rx_md);
    uhd_rx_metadata_make(&handler->rx_md_first);
    uhd_tx_metadata_make(&handler->tx_md, false, 0, 0, false, false);
  
    
    // Start low priority thread to receive async commands 
    handler->async_thread_running = true; 
    if (pthread_create(&handler->async_thread, NULL, async_thread, handler)) {
      perror("pthread_create");
      return -1; 
    }
    
    return 0;
  } else {
    return SRSLTE_ERROR_INVALID_INPUTS; 
  }
}


int rf_uhd_close(void *h)
{
  rf_uhd_stop_rx_stream(h);
  
  rf_uhd_handler_t *handler = (rf_uhd_handler_t*) h;
  
  uhd_tx_metadata_free(&handler->tx_md);
  uhd_rx_metadata_free(&handler->rx_md_first);
  uhd_rx_metadata_free(&handler->rx_md);
  uhd_meta_range_free(&handler->rx_gain_range);
  uhd_tx_streamer_free(&handler->tx_stream);
  uhd_rx_streamer_free(&handler->rx_stream);
  if (handler->has_rssi) {
    uhd_sensor_value_free(&handler->rssi_value);
  }
  handler->async_thread_running = false; 
  pthread_join(handler->async_thread, NULL); 
  uhd_usrp_free(&handler->usrp);
  
  /** Something else to close the USRP?? */
  return 0;
}

void rf_uhd_set_master_clock_rate(void *h, double rate) {
  rf_uhd_handler_t *handler = (rf_uhd_handler_t*) h;
  if (handler->dynamic_rate) {
    uhd_usrp_set_master_clock_rate(handler->usrp, rate, 0);
  }
}

bool rf_uhd_is_master_clock_dynamic(void *h) {
  rf_uhd_handler_t *handler = (rf_uhd_handler_t*) h;
  return handler->dynamic_rate;
}

double rf_uhd_set_rx_srate(void *h, double freq)
{
  rf_uhd_handler_t *handler = (rf_uhd_handler_t*) h;
  for (int i=0;i<handler->nof_rx_channels;i++) {
    uhd_usrp_set_rx_rate(handler->usrp, freq, i);
  }
  uhd_usrp_get_rx_rate(handler->usrp, 0, &freq);
  return freq; 
}

double rf_uhd_set_tx_srate(void *h, double freq)
{
  rf_uhd_handler_t *handler = (rf_uhd_handler_t*) h;
  for (int i=0;i<handler->nof_tx_channels;i++) {
    uhd_usrp_set_tx_rate(handler->usrp, freq, i);
  }
  uhd_usrp_get_tx_rate(handler->usrp, 0, &freq);
  handler->tx_rate = freq;
  return freq; 
}

double rf_uhd_set_rx_gain(void *h, double gain)
{
  rf_uhd_handler_t *handler = (rf_uhd_handler_t*) h;
  for (int i=0;i<handler->nof_rx_channels;i++) {
    uhd_usrp_set_rx_gain(handler->usrp, gain, i, "");
  }
  uhd_usrp_get_rx_gain(handler->usrp, 0, "", &gain);
  return gain;
}

double rf_uhd_set_tx_gain(void *h, double gain)
{
  rf_uhd_handler_t *handler = (rf_uhd_handler_t*) h;
  for (int i=0;i<handler->nof_tx_channels;i++) {
    uhd_usrp_set_tx_gain(handler->usrp, gain, i, "");
  }
  uhd_usrp_get_tx_gain(handler->usrp, 0, "", &gain);
  return gain;
}

double rf_uhd_get_rx_gain(void *h)
{
  rf_uhd_handler_t *handler = (rf_uhd_handler_t*) h;
  double gain; 
  uhd_usrp_get_rx_gain(handler->usrp, 0, "", &gain);
  return gain;
}

double rf_uhd_get_tx_gain(void *h)
{
  rf_uhd_handler_t *handler = (rf_uhd_handler_t*) h;
  double gain; 
  uhd_usrp_get_tx_gain(handler->usrp, 0, "", &gain);
  return gain;
}

double rf_uhd_set_rx_freq(void *h, double freq)
{
  uhd_tune_request_t tune_request = {
      .target_freq = freq,
      .rf_freq_policy = UHD_TUNE_REQUEST_POLICY_AUTO,
      .dsp_freq_policy = UHD_TUNE_REQUEST_POLICY_AUTO,
  };
  uhd_tune_result_t tune_result;
  rf_uhd_handler_t *handler = (rf_uhd_handler_t*) h;
  for (int i=0;i<handler->nof_rx_channels;i++) {
    uhd_usrp_set_rx_freq(handler->usrp, &tune_request, i, &tune_result);
  }
  uhd_usrp_get_rx_freq(handler->usrp, 0, &freq);
  return freq;
}

double rf_uhd_set_tx_freq(void *h, double freq)
{
  uhd_tune_request_t tune_request = {
      .target_freq = freq,
      .rf_freq_policy = UHD_TUNE_REQUEST_POLICY_AUTO,
      .dsp_freq_policy = UHD_TUNE_REQUEST_POLICY_AUTO,
  };
  uhd_tune_result_t tune_result;
  rf_uhd_handler_t *handler = (rf_uhd_handler_t*) h;
  for (int i=0;i<handler->nof_tx_channels;i++) {
    uhd_usrp_set_tx_freq(handler->usrp, &tune_request, i, &tune_result);
  }
  uhd_usrp_get_tx_freq(handler->usrp, 0, &freq);
  return freq;
}


void rf_uhd_get_time(void *h, time_t *secs, double *frac_secs) {
  rf_uhd_handler_t *handler = (rf_uhd_handler_t*) h;
  uhd_usrp_get_time_now(handler->usrp, 0, secs, frac_secs);
}

int rf_uhd_recv_with_time(void *h,
                    void *data,
                    uint32_t nsamples,
                    bool blocking,
                    time_t *secs,
                    double *frac_secs) 
{
  return rf_uhd_recv_with_time_multi(h, &data, nsamples, blocking, secs, frac_secs);
}

int rf_uhd_recv_with_time_multi(void *h,
                                void **data,
                                uint32_t nsamples,
                                bool blocking,
                                time_t *secs,
                                double *frac_secs) 
{
  
  rf_uhd_handler_t *handler = (rf_uhd_handler_t*) h;
  size_t rxd_samples;
  uhd_rx_metadata_handle *md = &handler->rx_md_first; 
  int trials = 0; 
  if (blocking) {
    int n = 0;
    do {
      size_t rx_samples = nsamples;
             
      if (rx_samples > nsamples - n) {
        rx_samples = nsamples - n; 
      }
      void *buffs_ptr[4]; 
      for (int i=0;i<handler->nof_rx_channels;i++) {
        cf_t *data_c = (cf_t*) data[i];
        buffs_ptr[i] = &data_c[n];        
      }

      uhd_error error = uhd_rx_streamer_recv(handler->rx_stream, buffs_ptr, 
                                             rx_samples, md, 1.0, false, &rxd_samples);
      if (error) {
        fprintf(stderr, "Error receiving from UHD: %d\n", error);
        return -1; 
      }
      md = &handler->rx_md; 
      n += rxd_samples;
      trials++;
      
      uhd_rx_metadata_error_code_t error_code;
      uhd_rx_metadata_error_code(*md, &error_code);
      if (error_code == UHD_RX_METADATA_ERROR_CODE_OVERFLOW) {
        log_overflow(handler);
      } else if (error_code == UHD_RX_METADATA_ERROR_CODE_LATE_COMMAND) {
        log_late(handler);
      } else if (error_code != UHD_RX_METADATA_ERROR_CODE_NONE) {
        fprintf(stderr, "Error code 0x%x was returned during streaming. Aborting.\n", error_code);
      }
      
    } while (n < nsamples && trials < 100);
  } else {
    return uhd_rx_streamer_recv(handler->rx_stream, data, 
                                nsamples, md, 0.0, false, &rxd_samples);
  }
  if (secs && frac_secs) {
    uhd_rx_metadata_time_spec(handler->rx_md_first, secs, frac_secs);
  }
  return nsamples;
}
                   
int rf_uhd_send_timed(void *h,
                     void *data,
                     int nsamples,
                     time_t secs,
                     double frac_secs,                      
                     bool has_time_spec,
                     bool blocking,
                     bool is_start_of_burst,
                     bool is_end_of_burst) 
{
  rf_uhd_handler_t* handler = (rf_uhd_handler_t*) h;
  
  size_t txd_samples;
  if (has_time_spec) {
    uhd_tx_metadata_set_time_spec(&handler->tx_md, secs, frac_secs);
  }
  int trials = 0; 
  if (blocking) {
    int n = 0;
    cf_t *data_c = (cf_t*) data;
    do {
      size_t tx_samples = handler->tx_nof_samples;
      
      // First packet is start of burst if so defined, others are never 
      if (n == 0) {
        uhd_tx_metadata_set_start(&handler->tx_md, is_start_of_burst);
      } else {
        uhd_tx_metadata_set_start(&handler->tx_md, false);
      }
      
      // middle packets are never end of burst, last one as defined
      if (nsamples - n > tx_samples) {
        uhd_tx_metadata_set_end(&handler->tx_md, false);
      } else {
        tx_samples = nsamples - n; 
        uhd_tx_metadata_set_end(&handler->tx_md, is_end_of_burst);
      }
      
      void *buff = (void*) &data_c[n];
      const void *buffs_ptr[4] = {buff, zero_mem, zero_mem, zero_mem};
      uhd_error error = uhd_tx_streamer_send(handler->tx_stream, buffs_ptr, 
                                             tx_samples, &handler->tx_md, 3.0, &txd_samples);
      if (error) {
        fprintf(stderr, "Error sending to UHD: %d\n", error);
        return -1; 
      }
      // Increase time spec 
      uhd_tx_metadata_add_time_spec(&handler->tx_md, txd_samples/handler->tx_rate);
      n += txd_samples;
      trials++;
    } while (n < nsamples && trials < 100);
    return nsamples;
  } else {
    const void *buffs_ptr[4] = {data, zero_mem, zero_mem, zero_mem};
    uhd_tx_metadata_set_start(&handler->tx_md, is_start_of_burst);
    uhd_tx_metadata_set_end(&handler->tx_md, is_end_of_burst);
    return uhd_tx_streamer_send(handler->tx_stream, buffs_ptr, nsamples, &handler->tx_md, 0.0, &txd_samples);
  }
}

