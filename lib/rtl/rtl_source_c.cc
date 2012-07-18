/* -*- c++ -*- */
/*
 * Copyright 2012 Dimitri Stolnikov <horiz0n@gmx.net>
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

#include "rtl_source_c.h"
#include <gr_io_signature.h>

#include <boost/assign.hpp>
#include <boost/format.hpp>
#include <boost/detail/endian.hpp>

#include <stdexcept>
#include <iostream>
#include <stdio.h>

#include <rtl-sdr.h>

#include <osmosdr_arg_helpers.h>

using namespace boost::assign;

#define BUF_SIZE  (16 * 32 * 512)
#define BUF_NUM   32
#define BUF_SKIP  1 // buffers to skip due to initial garbage

#define BYTES_PER_SAMPLE  2 // rtl device delivers 8 bit unsigned IQ data

/*
 * Create a new instance of rtl_source_c and return
 * a boost shared_ptr.  This is effectively the public constructor.
 */
rtl_source_c_sptr
make_rtl_source_c (const std::string &args)
{
  return gnuradio::get_initial_sptr(new rtl_source_c (args));
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
rtl_source_c::rtl_source_c (const std::string &args)
  : gr_sync_block ("rtl_source_c",
        gr_make_io_signature (MIN_IN, MAX_IN, sizeof (gr_complex)),
        gr_make_io_signature (MIN_OUT, MAX_OUT, sizeof (gr_complex))),
    _running(true),
    _auto_gain(false),
    _skipped(0)
{
  int ret;
  unsigned int dev_index = 0, rtl_freq = 0, tuner_freq = 0;

  dict_t dict = params_to_dict(args);

  if (dict.count("rtl"))
    dev_index = boost::lexical_cast< unsigned int >( dict["rtl"] );

  if (dict.count("rtl_xtal"))
    rtl_freq = (unsigned int)boost::lexical_cast< double >( dict["rtl_xtal"] );

  if (dict.count("tuner_xtal"))
    tuner_freq = (unsigned int)boost::lexical_cast< double >( dict["tuner_xtal"] );

  _buf_num = BUF_NUM;
  _buf_head = _buf_used = _buf_offset = 0;
  _samp_avail = BUF_SIZE / BYTES_PER_SAMPLE;

  if (dict.count("buffers")) {
    _buf_num = (unsigned int)boost::lexical_cast< double >( dict["buffers"] );
    if (0 == _buf_num)
      _buf_num = BUF_NUM;
    std::cerr << "Using " << _buf_num << " buffers of size " << BUF_SIZE << "."
              << std::endl;
  }

  // create a lookup table for gr_complex values
  for (unsigned int i = 0; i <= 0xffff; i++) {
#ifdef BOOST_LITTLE_ENDIAN
    _lut.push_back( gr_complex( (float(i & 0xff) - 127.5f) * (1.0f/128.0f),
                                (float(i >> 8) - 127.5f) * (1.0f/128.0f) ) );
#else // BOOST_BIG_ENDIAN
    _lut.push_back( gr_complex( (float(i >> 8) - 127.5f) * (1.0f/128.0f),
                                (float(i & 0xff) - 127.5f) * (1.0f/128.0f) ) );
#endif
  }

  if ( dev_index >= rtlsdr_get_device_count() )
    throw std::runtime_error("Wrong rtlsdr device index given.");

  std::cerr << "Using device #" << dev_index << ": "
            << rtlsdr_get_device_name(dev_index)
            << std::endl;

  _dev = NULL;
  ret = rtlsdr_open( &_dev, dev_index );
  if (ret < 0)
    throw std::runtime_error("Failed to open rtlsdr device.");

  if (rtl_freq > 0 || tuner_freq > 0) {
    if (rtl_freq)
      std::cerr << "Setting rtl clock to " << rtl_freq << " Hz." << std::endl;
    if (tuner_freq)
      std::cerr << "Setting tuner clock to " << tuner_freq << " Hz." << std::endl;

    ret = rtlsdr_set_xtal_freq( _dev, rtl_freq, tuner_freq );
    if (ret < 0)
      throw std::runtime_error(
        str(boost::format("Failed to set xtal frequencies. Error %d.") % ret ));
  }

  ret = rtlsdr_set_sample_rate( _dev, 1024000 );
  if (ret < 0)
    throw std::runtime_error("Failed to set default samplerate.");

  ret = rtlsdr_set_tuner_gain_mode(_dev, int(!_auto_gain));
  if (ret < 0)
    throw std::runtime_error("Failed to enable manual gain mode.");

  ret = rtlsdr_reset_buffer( _dev );
  if (ret < 0)
    throw std::runtime_error("Failed to reset usb buffers.");

  set_if_gain( 24 ); /* preset to a reasonable default (non-GRC use case) */

  _buf = (unsigned short **) malloc(_buf_num * sizeof(unsigned short *));

  for(unsigned int i = 0; i < _buf_num; ++i)
    _buf[i] = (unsigned short *) malloc(BUF_SIZE);

  _thread = gruel::thread(_rtlsdr_wait, this);
}

/*
 * Our virtual destructor.
 */
rtl_source_c::~rtl_source_c ()
{
  if (_dev) {
    _running = false;
    rtlsdr_cancel_async( _dev );
    _thread.timed_join( boost::posix_time::milliseconds(200) );
    rtlsdr_close( _dev );
    _dev = NULL;
  }

  for(unsigned int i = 0; i < _buf_num; ++i) {
    if (_buf[i])
      free(_buf[i]);
  }

  free(_buf);
  _buf = NULL;
}

void rtl_source_c::_rtlsdr_callback(unsigned char *buf, uint32_t len, void *ctx)
{
  rtl_source_c *obj = (rtl_source_c *)ctx;
  obj->rtlsdr_callback(buf, len);
}

void rtl_source_c::rtlsdr_callback(unsigned char *buf, uint32_t len)
{
  if (_skipped < BUF_SKIP) {
    _skipped++;
    return;
  }

  {
    boost::mutex::scoped_lock lock( _buf_mutex );

    int buf_tail = (_buf_head + _buf_used) % _buf_num;
    memcpy(_buf[buf_tail], buf, len);

    if (_buf_used == _buf_num) {
      printf("O"); fflush(stdout);
      _buf_head = (_buf_head + 1) % _buf_num;
    } else {
      _buf_used++;
    }
  }

  _buf_cond.notify_one();
}

void rtl_source_c::_rtlsdr_wait(rtl_source_c *obj)
{
  obj->rtlsdr_wait();
}

void rtl_source_c::rtlsdr_wait()
{
  int ret = rtlsdr_read_async( _dev, _rtlsdr_callback, (void *)this, 0, BUF_SIZE );

  _running = false;

  if ( ret != 0 )
    std::cerr << "rtlsdr_read_async returned with " << ret << std::endl;
}

int rtl_source_c::work( int noutput_items,
                        gr_vector_const_void_star &input_items,
                        gr_vector_void_star &output_items )
{
  gr_complex *out = (gr_complex *)output_items[0];

  {
    boost::mutex::scoped_lock lock( _buf_mutex );

    while (_buf_used < 3 && _running) // collect at least 3 buffers
      _buf_cond.wait( lock );
  }

  if (!_running)
    return WORK_DONE;

  unsigned short *buf = _buf[_buf_head] + _buf_offset;

  if (noutput_items <= _samp_avail) {
    for (int i = 0; i < noutput_items; ++i)
      *out++ = _lut[ *(buf + i) ];

    _buf_offset += noutput_items;
    _samp_avail -= noutput_items;
  } else {
    for (int i = 0; i < _samp_avail; ++i)
      *out++ = _lut[ *(buf + i) ];

    {
      boost::mutex::scoped_lock lock( _buf_mutex );

      _buf_head = (_buf_head + 1) % _buf_num;
      _buf_used--;
    }

    buf = _buf[_buf_head];

    int remaining = noutput_items - _samp_avail;

    for (int i = 0; i < remaining; ++i)
      *out++ = _lut[ *(buf + i) ];

    _buf_offset = remaining;
    _samp_avail = (BUF_SIZE / BYTES_PER_SAMPLE) - remaining;
  }

  return noutput_items;
}

std::vector<std::string> rtl_source_c::get_devices()
{
  std::vector<std::string> devices;

  for (unsigned int i = 0; i < rtlsdr_get_device_count(); i++) {
    std::string args = "rtl=" + boost::lexical_cast< std::string >( i );
    args += ",label='" + std::string(rtlsdr_get_device_name( i )) + "'";
    devices.push_back( args );
  }

  return devices;
}

size_t rtl_source_c::get_num_channels()
{
  return 1;
}

osmosdr::meta_range_t rtl_source_c::get_sample_rates()
{
  osmosdr::meta_range_t range;

  range += osmosdr::range_t( 250000 ); // known to work
  range += osmosdr::range_t( 1000000 ); // known to work
  range += osmosdr::range_t( 1024000 ); // known to work
  range += osmosdr::range_t( 1800000 ); // known to work
  range += osmosdr::range_t( 1920000 ); // known to work
  range += osmosdr::range_t( 2048000 ); // known to work
  range += osmosdr::range_t( 2400000 ); // known to work
  range += osmosdr::range_t( 2600000 ); // may work
  range += osmosdr::range_t( 2800000 ); // may work
  range += osmosdr::range_t( 3000000 ); // may work
  range += osmosdr::range_t( 3200000 ); // max rate

  return range;
}

double rtl_source_c::set_sample_rate(double rate)
{
  if (_dev) {
    rtlsdr_set_sample_rate( _dev, (uint32_t)rate );
  }

  return get_sample_rate();
}

double rtl_source_c::get_sample_rate()
{
  if (_dev)
    return (double)rtlsdr_get_sample_rate( _dev );

  return 0;
}

osmosdr::freq_range_t rtl_source_c::get_freq_range( size_t chan )
{
  osmosdr::freq_range_t range;

  if (_dev) {
    enum rtlsdr_tuner tuner = rtlsdr_get_tuner_type(_dev);

    if ( tuner == RTLSDR_TUNER_E4000 ) {
      /* there is a (temperature dependent) gap between 1100 to 1250 MHz */
      range += osmosdr::range_t( 50e6, 2.2e9 );
    } else if ( tuner == RTLSDR_TUNER_FC0012 ) {
      /* range += osmosdr::range_t( 50e6, 2.2e9 ); */
    } else if ( tuner == RTLSDR_TUNER_FC0013 ) {
      /* range += osmosdr::range_t( 50e6, 2.2e9 ); */
    } else if ( tuner == RTLSDR_TUNER_FC2580 ) {
      /* range += osmosdr::range_t( 50e6, 2.2e9 ); */
    }
  }

  return range;
}

double rtl_source_c::set_center_freq( double freq, size_t chan )
{
  if (_dev)
    rtlsdr_set_center_freq( _dev, (uint32_t)freq );

  return get_center_freq( chan );
}

double rtl_source_c::get_center_freq( size_t chan )
{
  if (_dev)
    return (double)rtlsdr_get_center_freq( _dev );

  return 0;
}

double rtl_source_c::set_freq_corr( double ppm, size_t chan )
{
  if ( _dev )
    rtlsdr_set_freq_correction( _dev, (int)ppm );

  return get_freq_corr( chan );
}

double rtl_source_c::get_freq_corr( size_t chan )
{
  if ( _dev )
    return (double)rtlsdr_get_freq_correction( _dev );

  return 0;
}

std::vector<std::string> rtl_source_c::get_gain_names( size_t chan )
{
  std::vector< std::string > gains;

  gains += "LNA";

  return gains;
}

osmosdr::gain_range_t rtl_source_c::get_gain_range( size_t chan )
{
  osmosdr::gain_range_t range;

  if (_dev) {
    int count = rtlsdr_get_tuner_gains(_dev, NULL);
    if (count > 0) {
      int* gains = new int[ count ];
      count = rtlsdr_get_tuner_gains(_dev, gains);
      for (int i = 0; i < count; i++)
        range += osmosdr::range_t( gains[i] / 10.0 );
      delete[] gains;
    }
  }

  return range;
}

osmosdr::gain_range_t rtl_source_c::get_gain_range( const std::string & name, size_t chan )
{
  return get_gain_range( chan );
}

bool rtl_source_c::set_gain_mode( bool automatic, size_t chan )
{
  if (_dev) {
    if (!rtlsdr_set_tuner_gain_mode(_dev, int(!automatic))) {
      _auto_gain = automatic;
    }
  }

  return get_gain_mode(chan);
}

bool rtl_source_c::get_gain_mode( size_t chan )
{
  return _auto_gain;
}

double rtl_source_c::set_gain( double gain, size_t chan )
{
  osmosdr::gain_range_t rf_gains = rtl_source_c::get_gain_range( chan );

  if (_dev) {
    rtlsdr_set_tuner_gain( _dev, int(rf_gains.clip(gain) * 10.0) );
  }

  return get_gain( chan );
}

double rtl_source_c::set_gain( double gain, const std::string & name, size_t chan)
{
  return set_gain( gain, chan );
}

double rtl_source_c::get_gain( size_t chan )
{
  if ( _dev )
    return ((double)rtlsdr_get_tuner_gain( _dev )) / 10.0;

  return 0;
}

double rtl_source_c::get_gain( const std::string & name, size_t chan )
{
  return get_gain( chan );
}

double rtl_source_c::set_if_gain(double gain, size_t chan)
{
  std::vector< osmosdr::gain_range_t > if_gains;

  if_gains += osmosdr::gain_range_t(-3, 6, 9);
  if_gains += osmosdr::gain_range_t(0, 9, 3);
  if_gains += osmosdr::gain_range_t(0, 9, 3);
  if_gains += osmosdr::gain_range_t(0, 2, 1);
  if_gains += osmosdr::gain_range_t(3, 15, 3);
  if_gains += osmosdr::gain_range_t(3, 15, 3);

  std::map< int, double > gains;

  /* initialize with min gains */
  for (unsigned int i = 0; i < if_gains.size(); i++) {
    gains[ i + 1 ] = if_gains[ i ].start();
  }

  for (int i = if_gains.size() - 1; i >= 0; i--) {
    osmosdr::gain_range_t range = if_gains[ i ];

    double error = gain;

    for( double g = range.start(); g <= range.stop(); g += range.step() ) {

      double sum = 0;
      for (int j = 0; j < int(gains.size()); j++) {
        if ( i == j )
          sum += g;
        else
          sum += gains[ j + 1 ];
      }

      double err = abs(gain - sum);
      if (err < error) {
        error = err;
        gains[ i + 1 ] = g;
      }
    }
  }
#if 0
  std::cerr << gain << " => "; double sum = 0;
  for (unsigned int i = 0; i < gains.size(); i++) {
    sum += gains[ i + 1 ];
    std::cerr << gains[ i + 1 ] << " ";
  }
  std::cerr << " = " << sum << std::endl;
#endif
  if (_dev) {
    for (unsigned int stage = 1; stage <= gains.size(); stage++) {
      rtlsdr_set_tuner_if_gain( _dev, stage, int(gains[ stage ] * 10.0));
    }
  }

  return gain;
}

std::vector< std::string > rtl_source_c::get_antennas( size_t chan )
{
  std::vector< std::string > antennas;

  antennas += get_antenna( chan );

  return antennas;
}

std::string rtl_source_c::set_antenna( const std::string & antenna, size_t chan )
{
  return get_antenna( chan );
}

std::string rtl_source_c::get_antenna( size_t chan )
{
  return "ANT";
}
