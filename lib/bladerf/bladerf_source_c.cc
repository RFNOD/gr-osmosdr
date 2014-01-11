/* -*- c++ -*- */
/*
 * Copyright 2013 Nuand LLC
 * Copyright 2013 Dimitri Stolnikov <horiz0n@gmx.net>
 *
 * GNU Radio is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3, or (at your option)
 * any later version.
 *
 * GNU Radio is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with GNU Radio; see the file COPYING.  If not, write to
 * the Free Software Foundation, Inc., 51 Franklin Street,
 * Boston, MA 02110-1301, USA.
 */

/*
 * config.h is generated by configure.  It contains the results
 * of probing for features, options etc.  It should be the first
 * file included in your .cc file.
 */
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <iostream>

#include <boost/assign.hpp>
#include <boost/format.hpp>
#include <boost/lexical_cast.hpp>

#include <gnuradio/io_signature.h>

#include "arg_helpers.h"
#include "bladerf_source_c.h"
#include "osmosdr/source.h"

/*
 * Default size of sample FIFO, in entries.
 */
#define BLADERF_SAMPLE_FIFO_SIZE (2 * 1024 * 1024)

#define BLADERF_SAMPLE_FIFO_MIN_SIZE  (3 * BLADERF_SAMPLE_BLOCK_SIZE)

using namespace boost::assign;

/*
 * Create a new instance of bladerf_source_c and return
 * a boost shared_ptr.  This is effectively the public constructor.
 */
bladerf_source_c_sptr make_bladerf_source_c (const std::string &args)
{
  return gnuradio::get_initial_sptr(new bladerf_source_c (args));
}

/*
 * Specify constraints on number of input and output streams.
 * This info is used to construct the input and output signatures
 * (2nd & 3rd args to gr_block's constructor).  The input and
 * output signatures are used by the runtime system to
 * check that a valid number and type of inputs and outputs
 * are connected to this block.  In this case, we accept
 * only 0 input and 1 output.
 */
static const int MIN_IN = 0;	// mininum number of input streams
static const int MAX_IN = 0;	// maximum number of input streams
static const int MIN_OUT = 1;	// minimum number of output streams
static const int MAX_OUT = 1;	// maximum number of output streams

/*
 * The private constructor
 */
bladerf_source_c::bladerf_source_c (const std::string &args)
  : gr::sync_block ("bladerf_source_c",
                    gr::io_signature::make (MIN_IN, MAX_IN, sizeof (gr_complex)),
                    gr::io_signature::make (MIN_OUT, MAX_OUT, sizeof (gr_complex)))
{
  int ret;
  size_t fifo_size;
  std::string device_name;

  dict_t dict = params_to_dict(args);

  init(dict, "source");

  fifo_size = BLADERF_SAMPLE_FIFO_SIZE;
  if (dict.count("fifo")) {
    try {
      fifo_size = boost::lexical_cast<size_t>(dict["fifo"]);
    } catch (const boost::bad_lexical_cast &e) {
      std::cerr << _pfx << "Warning: \"fifo\" value is invalid. Defaulting to "
                << fifo_size;
    }

    if (fifo_size < BLADERF_SAMPLE_FIFO_MIN_SIZE) {
      fifo_size = BLADERF_SAMPLE_FIFO_MIN_SIZE;
      std::cerr << _pfx << "Warning: \"fifo\" value is too small. Defaulting to "
                << BLADERF_SAMPLE_FIFO_MIN_SIZE;
    }
  }

  _fifo = new boost::circular_buffer<gr_complex>(fifo_size);
  if (!_fifo) {
    throw std::runtime_error( std::string(__FUNCTION__) + " " +
                              "Failed to allocate a sample FIFO!" );
  }

  if (dict.count("sampling"))
  {
    std::string sampling = dict["sampling"];

    std::cerr << _pfx << "Setting bladerf sampling to " << sampling << std::endl;
    if( sampling == "internal") {
      ret = bladerf_set_sampling( _dev.get(), BLADERF_SAMPLING_INTERNAL );
      if ( ret != 0 )
        std::cerr << _pfx << "Problem while setting sampling mode:"
                  << bladerf_strerror(ret) << std::endl;
    } else if( sampling == "external" ) {
      ret = bladerf_set_sampling( _dev.get(), BLADERF_SAMPLING_EXTERNAL );
      if ( ret != 0 )
        std::cerr << _pfx << "Problem while setting sampling mode:"
                  << bladerf_strerror(ret) << std::endl;
    } else {
        std::cerr << _pfx << "Invalid sampling mode " << sampling << std::endl;
    }
  }

  /* Set the range of LNA, G_LNA_RXFE[1:0] */
  _lna_range = osmosdr::gain_range_t( 0, 6, 3 );

  /* Set the range of VGA1, RFB_TIA_RXFE[6:0], nonlinear mapping done inside the lib */
  _vga1_range = osmosdr::gain_range_t( 5, 30, 1 );

  /* Set the range of VGA2 VGA2GAIN[4:0], not recommended to be used above 30dB */
  _vga2_range = osmosdr::gain_range_t( 0, 60, 3 );

  /* Initialize the stream */
  _buf_index = 0;
  ret = bladerf_init_stream( &_stream, _dev.get(), stream_callback,
                             &_buffers, _num_buffers, BLADERF_FORMAT_SC16_Q12,
                             _samples_per_buffer, _num_buffers, this );
  if ( ret != 0 )
    std::cerr << _pfx << "bladerf_init_stream failed: "
              << bladerf_strerror(ret) << std::endl;

  ret = bladerf_enable_module( _dev.get(), BLADERF_MODULE_RX, true );
  if ( ret != 0 )
    std::cerr << _pfx << "bladerf_enable_module failed:"
              << bladerf_strerror(ret) << std::endl;

  _thread = gr::thread::thread( boost::bind(&bladerf_source_c::read_task, this) );
}

/*
 * Our virtual destructor.
 */
bladerf_source_c::~bladerf_source_c ()
{
  int ret;

  set_running(false);
  _thread.join();

  ret = bladerf_enable_module( _dev.get(), BLADERF_MODULE_RX, false );
  if ( ret != 0 )
    std::cerr << _pfx << "bladerf_enable_module failed: "
              << bladerf_strerror(ret) << std::endl;

  /* Release stream resources */
  bladerf_deinit_stream(_stream);

  delete _fifo;
}

void *bladerf_source_c::stream_callback( struct bladerf *dev,
                                         struct bladerf_stream *stream,
                                         struct bladerf_metadata *metadata,
                                         void *samples,
                                         size_t num_samples,
                                         void *user_data )
{
  bladerf_source_c *obj = (bladerf_source_c *) user_data;

  if ( ! obj->is_running() )
    return NULL;

  return obj->stream_task( samples, num_samples );
}

/* Convert & push samples to the sample fifo */
void *bladerf_source_c::stream_task( void *samples, size_t num_samples )
{
  size_t i, n_avail, to_copy;
  int16_t *sample = (int16_t *)samples;
  void *ret;

  ret = _buffers[_buf_index];
  _buf_index = (_buf_index + 1) % _num_buffers;

  _fifo_lock.lock();

  n_avail = _fifo->capacity() - _fifo->size();
  to_copy = (n_avail < num_samples ? n_avail : num_samples);

  for(i = 0; i < to_copy; i++ ) {
    /* Mask valid bits only */
    *(sample) &= 0xfff;
    *(sample+1) &= 0xfff;

    /* Sign extend the 12-bit IQ values, if needed */
    if( (*sample) & 0x800 ) *(sample) |= 0xf000;
    if( *(sample+1) & 0x800 ) *(sample+1) |= 0xf000;

    /* Push sample to the fifo */
    _fifo->push_back( gr_complex( *sample * (1.0f/2048.0f),
                                  *(sample+1) * (1.0f/2048.0f) ) );

    /* offset to the next I+Q sample */
    sample += 2;
  }

  _fifo_lock.unlock();

  /* We have made some new samples available to the consumer in work() */
  if (to_copy) {
    //std::cerr << "+" << std::flush;
    _samp_avail.notify_one();
  }

  /* Indicate overrun, if neccesary */
  if (to_copy < num_samples)
    std::cerr << "O" << std::flush;

  return ret;
}

void bladerf_source_c::read_task()
{
  int status;

  set_running( true );

  /* Start stream and stay there until we kill the stream */
  status = bladerf_stream(_stream, BLADERF_MODULE_RX);

  if ( status < 0 ) {
      std::cerr << "Source stream error: " << bladerf_strerror(status) << std::endl;

      if ( status == BLADERF_ERR_TIMEOUT ) {
        std::cerr << _pfx << "Try adjusting your sample rate or the "
                  << "\"buffers\", \"buflen\", and \"transfers\" parameters. "
                  << std::endl;
      }
  }

  set_running( false );
}

/* Main work function, pull samples from the sample fifo */
int bladerf_source_c::work( int noutput_items,
                            gr_vector_const_void_star &input_items,
                            gr_vector_void_star &output_items )
{
  if ( ! is_running() )
    return WORK_DONE;

  if( noutput_items > 0 ) {
    gr_complex *out = (gr_complex *)output_items[0];

    boost::unique_lock<boost::mutex> lock(_fifo_lock);

    /* Wait until we have the requested number of samples */
    int n_samples_avail = _fifo->size();

    while (n_samples_avail < noutput_items) {
      _samp_avail.wait(lock);
      n_samples_avail = _fifo->size();
    }

    for(int i = 0; i < noutput_items; ++i) {
      out[i] = _fifo->at(0);
      _fifo->pop_front();
    }

    //std::cerr << "-" << std::flush;
  }

  return noutput_items;
}

std::vector<std::string> bladerf_source_c::get_devices()
{
  return bladerf_common::devices();
}

size_t bladerf_source_c::get_num_channels()
{
  /* We only support a single channel for each bladeRF */
  return 1;
}

osmosdr::meta_range_t bladerf_source_c::get_sample_rates()
{
  return sample_rates();
}

double bladerf_source_c::set_sample_rate( double rate )
{
  int ret;
  uint32_t actual;
  /* Set the Si5338 to be 2x this sample rate */

  /* Check to see if the sample rate is an integer */
  if( (uint32_t)round(rate) == (uint32_t)rate )
  {
    ret = bladerf_set_sample_rate( _dev.get(), BLADERF_MODULE_RX, (uint32_t)rate, &actual );
    if( ret ) {
      throw std::runtime_error( std::string(__FUNCTION__) + " " +
                                "has failed to set integer rate: " +
                                std::string(bladerf_strerror(ret)) );
    }
  } else {
    /* TODO: Fractional sample rate */
    ret = bladerf_set_sample_rate( _dev.get(), BLADERF_MODULE_RX, (uint32_t)rate, &actual );
    if( ret ) {
      throw std::runtime_error( std::string(__FUNCTION__) + " " +
                                "has failed to set fractional rate: " +
                                std::string(bladerf_strerror(ret)) );
    }
  }

  return get_sample_rate();
}

double bladerf_source_c::get_sample_rate()
{
  int ret;
  unsigned int rate = 0;

  ret = bladerf_get_sample_rate( _dev.get(), BLADERF_MODULE_RX, &rate );
  if( ret ) {
    throw std::runtime_error( std::string(__FUNCTION__) + " " +
                              "has failed to get sample rate, error " +
                              std::string(bladerf_strerror(ret)) );
  }

  return (double)rate;
}

osmosdr::freq_range_t bladerf_source_c::get_freq_range( size_t chan )
{
  return freq_range();
}

double bladerf_source_c::set_center_freq( double freq, size_t chan )
{
  int ret;

  /* Check frequency range */
  if( freq < get_freq_range( chan ).start() ||
      freq > get_freq_range( chan ).stop() ) {
    std::cerr << "Failed to set out of bound frequency: " << freq << std::endl;
  } else {
    ret = bladerf_set_frequency( _dev.get(), BLADERF_MODULE_RX, (uint32_t)freq );
    if( ret ) {
      throw std::runtime_error( std::string(__FUNCTION__) + " " +
                                "failed to set center frequency " +
                                boost::lexical_cast<std::string>(freq) + ": " +
                                std::string(bladerf_strerror(ret)) );
    }
  }

  return get_center_freq( chan );
}

double bladerf_source_c::get_center_freq( size_t chan )
{
  uint32_t freq;
  int ret;

  ret = bladerf_get_frequency( _dev.get(), BLADERF_MODULE_RX, &freq );
  if( ret ) {
    throw std::runtime_error( std::string(__FUNCTION__) + " " +
                              "failed to get center frequency: " +
                              std::string(bladerf_strerror(ret)) );
  }

  return (double)freq;
}

double bladerf_source_c::set_freq_corr( double ppm, size_t chan )
{
  /* TODO: Write the VCTCXO with a correction value (also changes TX ppm value!) */
  return get_freq_corr( chan );
}

double bladerf_source_c::get_freq_corr( size_t chan )
{
  /* TODO: Return back the frequency correction in ppm */
  return 0;
}

std::vector<std::string> bladerf_source_c::get_gain_names( size_t chan )
{
  std::vector< std::string > names;

  names += "LNA", "VGA1", "VGA2";

  return names;
}

osmosdr::gain_range_t bladerf_source_c::get_gain_range( size_t chan )
{
  /* TODO: This is an overall system gain range. Given the LNA, VGA1 and VGA2
  how much total gain can we have in the system */
  return get_gain_range( "LNA", chan ); /* we use only LNA here for now */
}

osmosdr::gain_range_t bladerf_source_c::get_gain_range( const std::string & name, size_t chan )
{
  osmosdr::gain_range_t range;

  if( name == "LNA" ) {
    range = _lna_range;
  } else if( name == "VGA1" ) {
    range = _vga1_range;
  } else if( name == "VGA2" ) {
    range = _vga2_range;
  } else {
    throw std::runtime_error( std::string(__FUNCTION__) + " " +
                              "requested an invalid gain element " + name );
  }

  return range;
}

bool bladerf_source_c::set_gain_mode( bool automatic, size_t chan )
{
  /* TODO: Implement AGC in the FPGA */
  return false;
}

bool bladerf_source_c::get_gain_mode( size_t chan )
{
  /* TODO: Read back AGC mode */
  return false;
}

double bladerf_source_c::set_gain( double gain, size_t chan )
{
  /* TODO: This is an overall system gain that has to be set */
  return set_gain( gain, "LNA", chan ); /* we use only LNA here for now */
}

double bladerf_source_c::set_gain( double gain, const std::string & name, size_t chan )
{
  int ret = 0;

  if( name == "LNA" ) {
    bladerf_lna_gain g;

    if ( gain >= 6.0f )
      g = BLADERF_LNA_GAIN_MAX;
    else if ( gain >= 3.0f )
      g = BLADERF_LNA_GAIN_MID;
    else /* gain < 3.0f */
      g = BLADERF_LNA_GAIN_BYPASS;

    ret = bladerf_set_lna_gain( _dev.get(), g );
  } else if( name == "VGA1" ) {
    ret = bladerf_set_rxvga1( _dev.get(), (int)gain );
  } else if( name == "VGA2" ) {
    ret = bladerf_set_rxvga2( _dev.get(), (int)gain );
  } else {
    throw std::runtime_error( std::string(__FUNCTION__) + " " +
                              "requested to set the gain "
                              "of an unknown gain element " + name );
  }

  /* Check for errors */
  if( ret ) {
    throw std::runtime_error( std::string(__FUNCTION__) + " " +
                              "could not set " + name + " gain: " +
                              std::string(bladerf_strerror(ret)) );
  }

  return get_gain( name, chan );
}

double bladerf_source_c::get_gain( size_t chan )
{
  /* TODO: This is an overall system gain that has to be set */
  return get_gain( "LNA", chan ); /* we use only LNA here for now */
}

double bladerf_source_c::get_gain( const std::string & name, size_t chan )
{
  int g;
  int ret = 0;

  if( name == "LNA" ) {
    bladerf_lna_gain lna_g;
    ret = bladerf_get_lna_gain( _dev.get(), &lna_g );
    g = lna_g == BLADERF_LNA_GAIN_BYPASS ? 0 : lna_g == BLADERF_LNA_GAIN_MID ? 3 : 6;
  } else if( name == "VGA1" ) {
    ret = bladerf_get_rxvga1( _dev.get(), &g );
  } else if( name == "VGA2" ) {
    ret = bladerf_get_rxvga2( _dev.get(), &g );
  } else {
    throw std::runtime_error( std::string(__FUNCTION__) + " " +
                              "requested to get the gain "
                              "of an unknown gain element " + name );
  }

  /* Check for errors */
  if( ret ) {
    throw std::runtime_error( std::string(__FUNCTION__) + " " +
                              "could not get " + name + " gain: " +
                              std::string(bladerf_strerror(ret)) );
  }

  return (double)g;
}

double bladerf_source_c::set_bb_gain( double gain, size_t chan )
{
  /* TODO: for RX, we should combine VGA1 & VGA2 which both are in BB path */
  osmosdr::gain_range_t bb_gains = get_gain_range( "VGA2", chan );

  double clip_gain = bb_gains.clip( gain, true );
  gain = set_gain( clip_gain, "VGA2", chan );

  return gain;
}

std::vector< std::string > bladerf_source_c::get_antennas( size_t chan )
{
  std::vector< std::string > antennas;

  antennas += get_antenna( chan );

  return antennas;
}

std::string bladerf_source_c::set_antenna( const std::string & antenna, size_t chan )
{
  return get_antenna( chan );
}

std::string bladerf_source_c::get_antenna( size_t chan )
{
  /* We only have a single receive antenna here */
  return "RX";
}

void bladerf_source_c::set_dc_offset_mode( int mode, size_t chan )
{
  if ( osmosdr::source::DCOffsetOff == mode ) {
    //_src->set_auto_dc_offset( false, chan );
    set_dc_offset( std::complex<double>(0.0, 0.0), chan ); /* reset to default for off-state */
  } else if ( osmosdr::source::DCOffsetManual == mode ) {
    //_src->set_auto_dc_offset( false, chan ); /* disable auto mode, but keep correcting with last known values */
  } else if ( osmosdr::source::DCOffsetAutomatic == mode ) {
    //_src->set_auto_dc_offset( true, chan );
    std::cerr << "Automatic DC correction mode is not implemented." << std::endl;
  }
}

void bladerf_source_c::set_dc_offset( const std::complex<double> &offset, size_t chan )
{
  int ret = 0;
  int16_t val_i,val_q;

  //the lms dc correction provides for 6 bits of DC correction and 1 sign bit
  //scale the correction appropriately
  val_i = (int16_t)(fabs(offset.real()) * BLADERF_RX_DC_RANGE);
  val_q = (int16_t)(fabs(offset.imag()) * BLADERF_RX_DC_RANGE);

  val_i = (offset.real() > 0) ? val_i : -val_i;
  val_q = (offset.imag() > 0) ? val_q : -val_q;

  ret = bladerf_set_correction(_dev.get(), BLADERF_IQ_CORR_RX_DC_I, val_i);
  ret |= bladerf_set_correction(_dev.get(), BLADERF_IQ_CORR_RX_DC_Q, val_q);

  if( ret ) {
    throw std::runtime_error( std::string(__FUNCTION__) + " " +
                              "could not set dc offset: " +
                              std::string(bladerf_strerror(ret)) );
  }
}

void bladerf_source_c::set_iq_balance_mode( int mode, size_t chan )
{
  if ( osmosdr::source::IQBalanceOff == mode ) {
    //_src->set_auto_iq_balance( false, chan );
    set_iq_balance( std::complex<double>(0.0, 0.0), chan ); /* reset to default for off-state */
  } else if ( osmosdr::source::IQBalanceManual == mode ) {
    //_src->set_auto_iq_balance( false, chan ); /* disable auto mode, but keep correcting with last known values */
  } else if ( osmosdr::source::IQBalanceAutomatic == mode ) {
    //_src->set_auto_iq_balance( true, chan );
    std::cerr << "Automatic IQ correction mode is not implemented." << std::endl;
  }
}

void bladerf_source_c::set_iq_balance( const std::complex<double> &balance, size_t chan )
{
  int ret = 0;
  int16_t val_gain,val_phase;

  //FPGA gain correction defines 0.0 as BLADERF_GAIN_ZERO, scale the offset range to +/- BLADERF_GAIN_RANGE 
  val_gain = (int16_t)(balance.real() * (int16_t)BLADERF_GAIN_RANGE) + BLADERF_GAIN_ZERO;
  //FPGA phase correction steps from -45 to 45 degrees
  val_phase = (int16_t)(balance.imag() * BLADERF_PHASE_RANGE);

  ret = bladerf_set_correction(_dev.get(), BLADERF_IQ_CORR_RX_GAIN, val_gain);
  ret |= bladerf_set_correction(_dev.get(), BLADERF_IQ_CORR_RX_PHASE, val_phase);

  if( ret ) {
    throw std::runtime_error( std::string(__FUNCTION__) + " " +
                              "could not set iq balance: " +
                              std::string(bladerf_strerror(ret)) );
  }
}

double bladerf_source_c::set_bandwidth( double bandwidth, size_t chan )
{
  int ret;
  uint32_t actual;

  if ( bandwidth == 0.0 ) /* bandwidth of 0 means automatic filter selection */
    bandwidth = get_sample_rate() * 0.75; /* select narrower filters to prevent aliasing */

  ret = bladerf_set_bandwidth( _dev.get(), BLADERF_MODULE_RX, (uint32_t)bandwidth, &actual );
  if( ret ) {
    throw std::runtime_error( std::string(__FUNCTION__) + " " +
                              "could not set bandwidth: " +
                              std::string(bladerf_strerror(ret)) );
  }

  return get_bandwidth();
}

double bladerf_source_c::get_bandwidth( size_t chan )
{
  uint32_t bandwidth;
  int ret;

  ret = bladerf_get_bandwidth( _dev.get(), BLADERF_MODULE_RX, &bandwidth );
  if( ret ) {
    throw std::runtime_error( std::string(__FUNCTION__) + " " +
                              "could not get bandwidth:" +
                              std::string(bladerf_strerror(ret)) );
  }

  return (double)bandwidth;
}

osmosdr::freq_range_t bladerf_source_c::get_bandwidth_range( size_t chan )
{
  return filter_bandwidths();
}
