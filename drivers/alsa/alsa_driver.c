/* -*- mode: c; c-file-style: "linux"; -*- */
/*
    Copyright (C) 2001 Paul Davis 

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program; if not, write to the Free Software
    Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.

    $Id$
*/

#include <math.h>
#include <stdio.h>
#include <memory.h>
#include <unistd.h>
#include <stdlib.h>
#include <errno.h>
#include <stdarg.h>
#include <signal.h>
#include <sys/types.h>
#include <regex.h>
#include <string.h>
 
#include <jack/internal.h>
#include <jack/engine.h>
#include <jack/messagebuffer.h>

#include <sysdeps/time.h>

#include "alsa_driver.h"
#include "hammerfall.h"
#include "hdsp.h"
#include "ice1712.h"
#include "usx2y.h"
#include "generic.h"

extern void store_work_time (int);
extern void store_wait_time (int);
extern void show_wait_times ();
extern void show_work_times ();

#undef DEBUG_WAKEUP

/* Delay (in process calls) before jackd will report an xrun */
#define XRUN_REPORT_DELAY 0


static void
alsa_driver_release_channel_dependent_memory (alsa_driver_t *driver)
{
	if (driver->playback_addr) {
		free (driver->playback_addr);
		driver->playback_addr = 0;
	}

	if (driver->capture_addr) {
		free (driver->capture_addr);
		driver->capture_addr = 0;
	}

	if (driver->silent) {
		free (driver->silent);
		driver->silent = 0;
	}

	if (driver->dither_state) {
		free (driver->dither_state);
		driver->dither_state = 0;
	}
}

static int
alsa_driver_check_capabilities (alsa_driver_t *driver)
{
	return 0;
}

static int
alsa_driver_check_card_type (alsa_driver_t *driver)
{
	int err;
	snd_ctl_card_info_t *card_info;
	char * ctl_name;
	regex_t expression;

	snd_ctl_card_info_alloca (&card_info);

	regcomp(&expression,"(plug)?hw:[0-9](,[0-9])?",REG_ICASE|REG_EXTENDED);
	
       	if (!regexec(&expression,driver->alsa_name_playback,0,NULL,0)) {
		/* the user wants a hw or plughw device, the ctl name
		 * should be hw:x where x is the card number */
	
		char tmp[5];
		strncpy(tmp,strstr(driver->alsa_name_playback,"hw"),4);
		tmp[4]='\0';
		printf("control device %s\n",tmp);
		ctl_name = strdup(tmp);
	} else {
		ctl_name = strdup(driver->alsa_name_playback);
	}

	// XXX: I don't know the "right" way to do this. Which to use
	// driver->alsa_name_playback or driver->alsa_name_capture.
	if ((err = snd_ctl_open (&driver->ctl_handle, ctl_name, 0)) < 0) {
		jack_error ("control open \"%s\" (%s)", ctl_name,
			    snd_strerror(err));
		return -1;
	}

	if ((err = snd_ctl_card_info(driver->ctl_handle, card_info)) < 0) {
		jack_error ("control hardware info \"%s\" (%s)",
			    driver->alsa_name_playback, snd_strerror (err));
		snd_ctl_close (driver->ctl_handle);
		return -1;
	}

	driver->alsa_driver = strdup(snd_ctl_card_info_get_driver (card_info));

	regfree(&expression);
	free(ctl_name);

	return alsa_driver_check_capabilities (driver);
}

static int
alsa_driver_hammerfall_hardware (alsa_driver_t *driver)
{
	driver->hw = jack_alsa_hammerfall_hw_new (driver);
	return 0;
}

static int
alsa_driver_hdsp_hardware (alsa_driver_t *driver)
{
	driver->hw = jack_alsa_hdsp_hw_new (driver);
	return 0;
}

static int
alsa_driver_ice1712_hardware (alsa_driver_t *driver)
{
        driver->hw = jack_alsa_ice1712_hw_new (driver);
        return 0;
}

static int
alsa_driver_usx2y_hardware (alsa_driver_t *driver)
{
	driver->hw = jack_alsa_usx2y_hw_new (driver);
	return 0;
}

static int
alsa_driver_generic_hardware (alsa_driver_t *driver)
{
	driver->hw = jack_alsa_generic_hw_new (driver);
	return 0;
}

static int
alsa_driver_hw_specific (alsa_driver_t *driver, int hw_monitoring,
			 int hw_metering)
{
	int err;

	if (!strcmp(driver->alsa_driver, "RME9652")) {
		if ((err = alsa_driver_hammerfall_hardware (driver)) != 0) {
			return err;
		}
	} else if (!strcmp(driver->alsa_driver, "H-DSP")) {
                if ((err = alsa_driver_hdsp_hardware (driver)) !=0) {
                        return err;
                }
	} else if (!strcmp(driver->alsa_driver, "ICE1712")) {
                if ((err = alsa_driver_ice1712_hardware (driver)) !=0) {
                        return err;
                }
	} else if (!strcmp(driver->alsa_driver, "USB US-X2Y")) {
		if ((err = alsa_driver_usx2y_hardware (driver)) !=0) {
				return err;
		}
	} else {
	        if ((err = alsa_driver_generic_hardware (driver)) != 0) {
			return err;
		}
	}

	if (driver->hw->capabilities & Cap_HardwareMonitoring) {
		driver->has_hw_monitoring = TRUE;
		/* XXX need to ensure that this is really FALSE or
		 * TRUE or whatever*/
		driver->hw_monitoring = hw_monitoring;
	} else {
		driver->has_hw_monitoring = FALSE;
		driver->hw_monitoring = FALSE;
	}

	if (driver->hw->capabilities & Cap_ClockLockReporting) {
		driver->has_clock_sync_reporting = TRUE;
	} else {
		driver->has_clock_sync_reporting = FALSE;
	}

	if (driver->hw->capabilities & Cap_HardwareMetering) {
		driver->has_hw_metering = TRUE;
		driver->hw_metering = hw_metering;
	} else {
		driver->has_hw_metering = FALSE;
		driver->hw_metering = FALSE;
	}

	return 0;
}

static void
alsa_driver_setup_io_function_pointers (alsa_driver_t *driver)
{
	switch (driver->playback_sample_bytes) {
	case 2:
		if (driver->playback_interleaved) {
			driver->channel_copy = memcpy_interleave_d16_s16;
		} else {
			driver->channel_copy = memcpy_fake;
		}

		switch (driver->dither) {
			case Rectangular:
			printf("Rectangular dithering at 16 bits\n");
			driver->write_via_copy = sample_move_dither_rect_d16_sS;
			break;

			case Triangular:
			printf("Triangular dithering at 16 bits\n");
			driver->write_via_copy = sample_move_dither_tri_d16_sS;
			break;

			case Shaped:
			printf("Noise-shaped dithering at 16 bits\n");
			driver->write_via_copy =
				sample_move_dither_shaped_d16_sS;
			break;

			default:
			driver->write_via_copy = sample_move_d16_sS;
			break;
		}
		break;

	case 3:
		if (driver->playback_interleaved) {
			driver->channel_copy = memcpy_interleave_d24_s24;
		} else {
			driver->channel_copy = memcpy_fake;
		}

		switch (driver->dither) {
			case Rectangular:
			printf("Rectangular dithering at 16 bits\n");
			driver->write_via_copy = sample_move_dither_rect_d24_sS;
			break;

			case Triangular:
			printf("Triangular dithering at 16 bits\n");
			driver->write_via_copy = sample_move_dither_tri_d24_sS;
			break;

			case Shaped:
			printf("Noise-shaped dithering at 16 bits\n");
			driver->write_via_copy = sample_move_dither_shaped_d24_sS;
			break;

			default:
			driver->write_via_copy = sample_move_d24_sS;
			break;
		}
		break;

	case 4:
		if (driver->playback_interleaved) {
			driver->channel_copy = memcpy_interleave_d32_s32;
		} else {
			driver->channel_copy = memcpy_fake;
		}
		
		switch (driver->dither) {
			case Rectangular:
			printf("Rectangular dithering at 16 bits\n");
			driver->write_via_copy =
				sample_move_dither_rect_d32u24_sS;
			break;

			case Triangular:
			printf("Triangular dithering at 16 bits\n");
			driver->write_via_copy =
				sample_move_dither_tri_d32u24_sS;
			break;

			case Shaped:
			printf("Noise-shaped dithering at 16 bits\n");
			driver->write_via_copy =
				sample_move_dither_shaped_d32u24_sS;
			break;

			default:
			driver->write_via_copy = sample_move_d32u24_sS;
			break;
		}
		break;
	}

	switch (driver->capture_sample_bytes) {
	case 2:
		driver->read_via_copy = sample_move_dS_s16;
		break;
	case 3:
		driver->read_via_copy = sample_move_dS_s24;
		break;
	case 4:
		driver->read_via_copy = sample_move_dS_s32u24;
		break;
	}
}

static int
alsa_driver_configure_stream (alsa_driver_t *driver, char *device_name,
			      const char *stream_name,
			      snd_pcm_t *handle, 
			      snd_pcm_hw_params_t *hw_params, 
			      snd_pcm_sw_params_t *sw_params, 
			      unsigned int *nperiodsp,
			      unsigned long *nchns,unsigned long sample_width)
{
	int err, format;
	snd_pcm_uframes_t stop_th;
	static struct {
		char Name[16];
		snd_pcm_format_t format;
	} formats[] = {
		{"32bit", SND_PCM_FORMAT_S32},
		{"24bit", SND_PCM_FORMAT_S24_3},
		{"16bit", SND_PCM_FORMAT_S16},
	};
#define NOFORMATS (sizeof(formats)/sizeof(formats[0]))
	
	if ((err = snd_pcm_hw_params_any (handle, hw_params)) < 0)  {
		jack_error ("ALSA: no playback configurations available (%s)",
			    snd_strerror (err));
		return -1;
	}

	if ((err = snd_pcm_hw_params_set_periods_integer (handle, hw_params))
	    < 0) {
		jack_error ("ALSA: cannot restrict period size to integral"
			    " value.");
		return -1;
	}

	if ((err = snd_pcm_hw_params_set_access (
		     handle, hw_params, SND_PCM_ACCESS_MMAP_NONINTERLEAVED))
	    < 0) {
		if ((err = snd_pcm_hw_params_set_access (
			     handle, hw_params,
			     SND_PCM_ACCESS_MMAP_INTERLEAVED)) < 0) {
			jack_error ("ALSA: mmap-based access is not possible"
				    " for the %s "
				    "stream of this audio interface",
				    stream_name);
			return -1;
		}
	}

	format = sample_width == 4 ? 0 : NOFORMATS - 1;
	while (1) {
		if ((err = snd_pcm_hw_params_set_format (handle, hw_params, formats[format].format)) < 0) {
			int failed_format = format; 
			if (sample_width == 4 ? format++ < NOFORMATS - 1 : format-- > 0) {
				jack_error ("Couldn't open %s for %s samples"
					    " trying %s instead", device_name, formats[failed_format].Name, formats[format].Name);
			} else {
				jack_error ("Sorry. The audio interface \"%s\""
					    " doesn't support any of the"
					    " hardware sample formats that"
					    " JACK's alsa-driver can use.",
					    device_name);
				return -1;
			}
		} else
			break;
	} 

	if ((err = snd_pcm_hw_params_set_rate_near (handle, hw_params,
						    driver->frame_rate, 0))
	    < 0) {
		jack_error ("ALSA: cannot set sample/frame rate to %"
			    PRIu32 " for %s", driver->frame_rate,
			    stream_name);
		return -1;
	}
	if (!*nchns) {
		/*if not user-specified, try to find the maximum
		 * number of channels */
		*nchns = snd_pcm_hw_params_get_channels_max (hw_params);

		if (*nchns > 1024) { 

			/* the hapless user is an unwitting victim of
			   the "default" ALSA PCM device, which can
			   support up to 16 million channels. since
			   they can't be bothered to set up a proper
			   default device, limit the number of
			   channels for them to a sane default.
			*/

			jack_error (
"You appear to be using the ALSA software \"plug\" layer, probably\n"
"a result of using the \"default\" ALSA device. This is less\n"
"efficient than it could be. Consider using a hardware device\n"
"instead rather than using the plug layer. Usually the name of the\n"
"hardware device that corresponds to the first sound card is hw:0\n"
				);
			*nchns = 2;  
		}
	}				

	if ((err = snd_pcm_hw_params_set_channels (handle, hw_params,
						   *nchns)) < 0) {
		jack_error ("ALSA: cannot set channel count to %u for %s",
			    *nchns, stream_name);
		return -1;
	}
	
	if ((err = snd_pcm_hw_params_set_period_size (handle, hw_params,
						      driver->frames_per_cycle,
						      0))
	    < 0) {
		jack_error ("ALSA: cannot set period size to %" PRIu32
			    " frames for %s", driver->frames_per_cycle,
			    stream_name);
		return -1;
	}

	*nperiodsp = driver->user_nperiods;
	snd_pcm_hw_params_set_periods_min (handle, hw_params, nperiodsp, NULL);
	*nperiodsp = driver->user_nperiods;
	if ((*nperiodsp = snd_pcm_hw_params_set_periods_near (handle, hw_params,
							      *nperiodsp, NULL))
	    < 0) {
		jack_error ("ALSA: cannot set number of periods to %u for %s",
			    *nperiodsp, stream_name);
		return -1;
	}

	if (*nperiodsp < driver->user_nperiods) {
		jack_error ("ALSA: got smaller periods %u than %u for %s",
			    *nperiodsp, (unsigned int)driver->user_nperiods, stream_name);
		return -1;
	}
	fprintf(stderr, "nperiods = %d for %s\n", *nperiodsp, stream_name);
	
	if (!jack_power_of_two(driver->frames_per_cycle)) {
		jack_error("JACK: frames must be a power of two "
			   "(64, 512, 1024, ...)\n");
		return -1;
	}

	if ((err = snd_pcm_hw_params_set_buffer_size (handle, hw_params,
						      *nperiodsp *
						      driver->frames_per_cycle))
	    < 0) {
		jack_error ("ALSA: cannot set buffer length to %" PRIu32
			    " for %s",
			    *nperiodsp * driver->frames_per_cycle,
			    stream_name);
		return -1;
	}

	if ((err = snd_pcm_hw_params (handle, hw_params)) < 0) {
		jack_error ("ALSA: cannot set hardware parameters for %s",
			    stream_name);
		return -1;
	}

	snd_pcm_sw_params_current (handle, sw_params);

	if ((err = snd_pcm_sw_params_set_start_threshold (handle, sw_params,
							  0U)) < 0) {
		jack_error ("ALSA: cannot set start mode for %s", stream_name);
		return -1;
	}

	stop_th = *nperiodsp * driver->frames_per_cycle;
	if (driver->soft_mode) {
		stop_th = (snd_pcm_uframes_t)-1;
	}
	
	if ((err = snd_pcm_sw_params_set_stop_threshold (
		     handle, sw_params, stop_th)) < 0) {
		jack_error ("ALSA: cannot set stop mode for %s",
			    stream_name);
		return -1;
	}

	if ((err = snd_pcm_sw_params_set_silence_threshold (
		     handle, sw_params, 0)) < 0) {
		jack_error ("ALSA: cannot set silence threshold for %s",
			    stream_name);
		return -1;
	}

#if 0
	fprintf (stderr, "set silence size to %lu * %lu = %lu\n",
		 driver->frames_per_cycle, *nperiodsp,
		 driver->frames_per_cycle * *nperiodsp);

	if ((err = snd_pcm_sw_params_set_silence_size (
		     handle, sw_params,
		     driver->frames_per_cycle * *nperiodsp)) < 0) {
		jack_error ("ALSA: cannot set silence size for %s",
			    stream_name);
		return -1;
	}
#endif

	if (handle == driver->playback_handle)
		err = snd_pcm_sw_params_set_avail_min (
			handle, sw_params,
			driver->frames_per_cycle * (*nperiodsp - driver->user_nperiods + 1));
	else
		err = snd_pcm_sw_params_set_avail_min (
			handle, sw_params, driver->frames_per_cycle);
			
	if (err < 0) {
		jack_error ("ALSA: cannot set avail min for %s", stream_name);
		return -1;
	}

	if ((err = snd_pcm_sw_params (handle, sw_params)) < 0) {
		jack_error ("ALSA: cannot set software parameters for %s\n",
			    stream_name);
		return -1;
	}

	return 0;
}

static int 
alsa_driver_set_parameters (alsa_driver_t *driver,
			    jack_nframes_t frames_per_cycle,
			    jack_nframes_t user_nperiods,
			    jack_nframes_t rate)
{
	int dir;
	unsigned int p_period_size = 0;
	unsigned int c_period_size = 0;
	channel_t chn;
	unsigned int pr = 0;
	unsigned int cr = 0;

	driver->frame_rate = rate;
	driver->frames_per_cycle = frames_per_cycle;
	driver->user_nperiods = user_nperiods;

	fprintf (stderr, "configuring for %" PRIu32 "Hz, period = %"
		 PRIu32 " frames, buffer = %" PRIu32 " periods\n",
		 rate, frames_per_cycle, user_nperiods);
	
	if (driver->capture_handle) {
		if (alsa_driver_configure_stream (
			    driver,
			    driver->alsa_name_capture,
			    "capture", 
			    driver->capture_handle,
			    driver->capture_hw_params,
			    driver->capture_sw_params,
			    &driver->capture_nperiods,
			    &driver->capture_nchannels,
			    driver->capture_sample_bytes)) {
			jack_error ("ALSA: cannot configure capture channel");
			return -1;
		}
	}

	if (driver->playback_handle) {
		if (alsa_driver_configure_stream (
			    driver,
			    driver->alsa_name_playback,
			    "playback",
			    driver->playback_handle,
			    driver->playback_hw_params,
			    driver->playback_sw_params,
			    &driver->playback_nperiods,
			    &driver->playback_nchannels,
			    driver->playback_sample_bytes)) {
			jack_error ("ALSA: cannot configure playback channel");
			return -1;
		}
	}
	
	/* check the rate, since thats rather important */

	if (driver->playback_handle) {
		int dir;
		pr = snd_pcm_hw_params_get_rate (driver->playback_hw_params, &dir);
	}

	if (driver->capture_handle) {
		int dir;
		cr = snd_pcm_hw_params_get_rate (driver->capture_hw_params, &dir);
	}

	if (driver->capture_handle && driver->playback_handle) {
		if (cr != pr) {
			jack_error ("playback and capture sample rates do not match (%d vs. %d)",
				    pr, cr);
		}

		/* only change if *both* capture and playback rates don't match requested
		 * certain hardware actually still works properly in full-duplex with
		 * slightly different rate values between adc and dac
		 */
		if (cr != driver->frame_rate && pr != driver->frame_rate) {
			jack_error ("sample rate in use (%d Hz) does not match requested rate (%d Hz)",
				    cr, driver->frame_rate);
			driver->frame_rate = cr;
		}
		
	}
	else if (driver->capture_handle && cr != driver->frame_rate) {
		jack_error ("capture sample rate in use (%d Hz) does not match requested rate (%d Hz)",
			    cr, driver->frame_rate);
		driver->frame_rate = cr;
	}
	else if (driver->playback_handle && pr != driver->frame_rate) {
		jack_error ("playback sample rate in use (%d Hz) does not match requested rate (%d Hz)",
			    pr, driver->frame_rate);
		driver->frame_rate = pr;
	}


	/* check the fragment size, since thats non-negotiable */
	
	if (driver->playback_handle) {
		p_period_size =
			snd_pcm_hw_params_get_period_size (
				driver->playback_hw_params, &dir);
		driver->playback_sample_format = (snd_pcm_format_t)
			snd_pcm_hw_params_get_format (
				driver->playback_hw_params);
		driver->playback_interleaved =
			(snd_pcm_hw_params_get_access (
				driver->playback_hw_params)
			 == SND_PCM_ACCESS_MMAP_INTERLEAVED);

		if (p_period_size != driver->frames_per_cycle) {
			jack_error ("alsa_pcm: requested an interrupt every %"
				    PRIu32
				    " frames but got %u frames for playback",
				    driver->frames_per_cycle, p_period_size);
			return -1;
		}
	}

	if (driver->capture_handle) {
		c_period_size =
			snd_pcm_hw_params_get_period_size (
				driver->capture_hw_params, &dir);
		driver->capture_sample_format = (snd_pcm_format_t)
			snd_pcm_hw_params_get_format (
				driver->capture_hw_params);
		driver->capture_interleaved =
			(snd_pcm_hw_params_get_access (
				driver->capture_hw_params)
			 == SND_PCM_ACCESS_MMAP_INTERLEAVED);
	
		if (c_period_size != driver->frames_per_cycle) {
			jack_error ("alsa_pcm: requested an interrupt every %"
				    PRIu32
				    " frames but got %uc frames for capture",
				    driver->frames_per_cycle, p_period_size);
			return -1;
		}
	}

	driver->playback_sample_bytes =
		snd_pcm_format_physical_width (driver->playback_sample_format)
		/ 8;
	driver->capture_sample_bytes =
		snd_pcm_format_physical_width (driver->capture_sample_format)
		/ 8;

	if (driver->playback_handle) {
		switch (driver->playback_sample_format) {
		case SND_PCM_FORMAT_S32_LE:
		case SND_PCM_FORMAT_S24_3:
		case SND_PCM_FORMAT_S16_LE:
		case SND_PCM_FORMAT_S32_BE:
		case SND_PCM_FORMAT_S16_BE:
			break;

		default:
			jack_error ("programming error: unhandled format "
				    "type for playback");
			exit (1);
		}
	}

	if (driver->capture_handle) {
		switch (driver->capture_sample_format) {
		case SND_PCM_FORMAT_S32_LE:
		case SND_PCM_FORMAT_S24_3:
		case SND_PCM_FORMAT_S16_LE:
		case SND_PCM_FORMAT_S32_BE:
		case SND_PCM_FORMAT_S16_BE:
			break;

		default:
			jack_error ("programming error: unhandled format "
				    "type for capture");
			exit (1);
		}
	}

	if (driver->playback_interleaved) {
		const snd_pcm_channel_area_t *my_areas;
		snd_pcm_uframes_t offset, frames;
		if (snd_pcm_mmap_begin(driver->playback_handle,
				       &my_areas, &offset, &frames) < 0) {
			jack_error ("ALSA: %s: mmap areas info error",
				    driver->alsa_name_playback);
			return -1;
		}
		driver->playback_interleave_skip = my_areas[0].step / 8;
		driver->interleave_unit =
			snd_pcm_format_physical_width (
				driver->playback_sample_format) / 8;
	} else {
		driver->interleave_unit = 0;  /* NOT USED */
		driver->playback_interleave_skip =
			snd_pcm_format_physical_width (
				driver->playback_sample_format) / 8;
	}

	if (driver->capture_interleaved) {
		const snd_pcm_channel_area_t *my_areas;
		snd_pcm_uframes_t offset, frames;
		if (snd_pcm_mmap_begin(driver->capture_handle,
				       &my_areas, &offset, &frames) < 0) {
			jack_error ("ALSA: %s: mmap areas info error",
				    driver->alsa_name_capture);
			return -1;
		}
		driver->capture_interleave_skip = my_areas[0].step / 8;
	} else {
		driver->capture_interleave_skip =
			snd_pcm_format_physical_width (
			driver->capture_sample_format) / 8;
	}

	if (driver->playback_nchannels > driver->capture_nchannels) {
		driver->max_nchannels = driver->playback_nchannels;
		driver->user_nchannels = driver->capture_nchannels;
	} else {
		driver->max_nchannels = driver->capture_nchannels;
		driver->user_nchannels = driver->playback_nchannels;
	}

	alsa_driver_setup_io_function_pointers (driver);

	/* Allocate and initialize structures that rely on the
	   channels counts.

	   Set up the bit pattern that is used to record which
	   channels require action on every cycle. any bits that are
	   not set after the engine's process() call indicate channels
	   that potentially need to be silenced.

	   XXX this is limited to <wordsize> channels. Use a bitset
	   type instead.
	*/

	driver->channel_done_bits = 0;

	if (driver->playback_handle) {
		driver->playback_addr = (char **)
			malloc (sizeof (char *) * driver->playback_nchannels);
		memset (driver->playback_addr, 0,
			sizeof (char *) * driver->playback_nchannels);
		driver->silent = (unsigned long *)
			malloc (sizeof (unsigned long)
				* driver->playback_nchannels);
		
		for (chn = 0; chn < driver->playback_nchannels; chn++) {
			driver->silent[chn] = 0;
		}

		for (chn = 0; chn < driver->playback_nchannels; chn++) {
			driver->channel_done_bits |= (1<<chn);
		}

		driver->dither_state = (dither_state_t *)
			calloc ( driver->playback_nchannels,
				 sizeof (dither_state_t));
	}

	if (driver->capture_handle) {
		driver->capture_addr = (char **)
			malloc (sizeof (char *) * driver->capture_nchannels);
		memset (driver->capture_addr, 0,
			sizeof (char *) * driver->capture_nchannels);
	}

	driver->clock_sync_data = (ClockSyncStatus *)
		malloc (sizeof (ClockSyncStatus) * driver->max_nchannels);

	driver->period_usecs =
		(jack_time_t) floor ((((float) driver->frames_per_cycle) /
				      driver->frame_rate) * 1000000.0f);
	driver->poll_timeout = (int) floor (1.5f * driver->period_usecs);

	if (driver->engine) {
		driver->engine->set_buffer_size (driver->engine,
						 driver->frames_per_cycle);
	}

	return 0;
}	

static int
alsa_driver_reset_parameters (alsa_driver_t *driver,
			      jack_nframes_t frames_per_cycle,
			      jack_nframes_t user_nperiods,
			      jack_nframes_t rate)
{
	/* XXX unregister old ports ? */
	alsa_driver_release_channel_dependent_memory (driver);
	return alsa_driver_set_parameters (driver,
					   frames_per_cycle,
					   user_nperiods, rate);
}

static int
alsa_driver_get_channel_addresses (alsa_driver_t *driver,
				   snd_pcm_uframes_t *capture_avail,
				   snd_pcm_uframes_t *playback_avail,
				   snd_pcm_uframes_t *capture_offset,
				   snd_pcm_uframes_t *playback_offset)
{
	unsigned long err;
	channel_t chn;

	if (capture_avail) {
		if ((err = snd_pcm_mmap_begin (
			     driver->capture_handle, &driver->capture_areas,
			     (snd_pcm_uframes_t *) capture_offset, 
			     (snd_pcm_uframes_t *) capture_avail)) < 0) {
			jack_error ("ALSA: %s: mmap areas info error",
				    driver->alsa_name_capture);
			return -1;
		}
		
		for (chn = 0; chn < driver->capture_nchannels; chn++) {
			const snd_pcm_channel_area_t *a =
				&driver->capture_areas[chn];
			driver->capture_addr[chn] = (char *) a->addr
				+ ((a->first + a->step * *capture_offset) / 8);
		}
	} 

	if (playback_avail) {
		if ((err = snd_pcm_mmap_begin (
			     driver->playback_handle, &driver->playback_areas, 
			     (snd_pcm_uframes_t *) playback_offset, 
			     (snd_pcm_uframes_t *) playback_avail)) < 0) {
			jack_error ("ALSA: %s: mmap areas info error ",
				    driver->alsa_name_playback);
			return -1;
		}
		
		for (chn = 0; chn < driver->playback_nchannels; chn++) {
			const snd_pcm_channel_area_t *a =
				&driver->playback_areas[chn];
			driver->playback_addr[chn] = (char *) a->addr
				+ ((a->first + a->step * *playback_offset) / 8);
		}
	} 
	
	return 0;
}

static int
alsa_driver_start (alsa_driver_t *driver)
{
	int err;
	snd_pcm_uframes_t poffset, pavail;
	channel_t chn;

	driver->poll_last = 0;
	driver->poll_next = 0;

	if (driver->playback_handle) {
		if ((err = snd_pcm_prepare (driver->playback_handle)) < 0) {
			jack_error ("ALSA: prepare error for playback on "
				    "\"%s\" (%s)", driver->alsa_name_playback,
				    snd_strerror(err));
			return -1;
		}
	}

	if ((driver->capture_handle && driver->capture_and_playback_not_synced)
	    || !driver->playback_handle) {
		if ((err = snd_pcm_prepare (driver->capture_handle)) < 0) {
			jack_error ("ALSA: prepare error for capture on \"%s\""
				    " (%s)", driver->alsa_name_capture,
				    snd_strerror(err));
			return -1;
		}
	}

	if (driver->hw_monitoring) {
		if (driver->input_monitor_mask || driver->all_monitor_in) {
			if (driver->all_monitor_in) {
				driver->hw->set_input_monitor_mask (driver->hw, ~0U);
			} else {
				driver->hw->set_input_monitor_mask (
					driver->hw, driver->input_monitor_mask);
			}
		} else {
			driver->hw->set_input_monitor_mask (driver->hw,
							    driver->input_monitor_mask);
		}
	}

	if (driver->playback_handle) {
		driver->playback_nfds =
			snd_pcm_poll_descriptors_count (driver->playback_handle);
	} else {
		driver->playback_nfds = 0;
	}

	if (driver->capture_handle) {
		driver->capture_nfds =
			snd_pcm_poll_descriptors_count (driver->capture_handle);
	} else {
		driver->capture_nfds = 0;
	}

	if (driver->pfd) {
		free (driver->pfd);
	}

	driver->pfd = (struct pollfd *)
		malloc (sizeof (struct pollfd) * 
			(driver->playback_nfds + driver->capture_nfds + 2));

	if (driver->playback_handle) {
		/* fill playback buffer with zeroes, and mark 
		   all fragments as having data.
		*/
		
		pavail = snd_pcm_avail_update (driver->playback_handle);

		if (pavail !=
		    driver->frames_per_cycle * driver->playback_nperiods) {
			jack_error ("ALSA: full buffer not available at start");
			return -1;
		}
	
		if (alsa_driver_get_channel_addresses (driver,
					0, &pavail, 0, &poffset)) {
			return -1;
		}

		/* XXX this is cheating. ALSA offers no guarantee that
		   we can access the entire buffer at any one time. It
		   works on most hardware tested so far, however, buts
		   its a liability in the long run. I think that
		   alsa-lib may have a better function for doing this
		   here, where the goal is to silence the entire
		   buffer.
		*/
		
		for (chn = 0; chn < driver->playback_nchannels; chn++) {
			alsa_driver_silence_on_channel (
				driver, chn,
				driver->user_nperiods
				* driver->frames_per_cycle);
		}
		
		snd_pcm_mmap_commit (driver->playback_handle, poffset,
				     driver->user_nperiods
				     * driver->frames_per_cycle);
		
		if ((err = snd_pcm_start (driver->playback_handle)) < 0) {
			jack_error ("ALSA: could not start playback (%s)",
				    snd_strerror (err));
			return -1;
		}
	}

	if ((driver->capture_handle && driver->capture_and_playback_not_synced)
	    || !driver->playback_handle) {
		if ((err = snd_pcm_start (driver->capture_handle)) < 0) {
			jack_error ("ALSA: could not start capture (%s)",
				    snd_strerror (err));
			return -1;
		}
	}
			
	return 0;
}

static int
alsa_driver_stop (alsa_driver_t *driver)
{
	int err;
	JSList* node;
	int chn;

	/* silence all capture port buffers, because we might
	   be entering offline mode.
	*/

	for (chn = 0, node = driver->capture_ports; node;
	     node = jack_slist_next (node), chn++) {

		jack_port_t* port;
		char* buf;
		jack_nframes_t nframes = driver->engine->control->buffer_size;

		port = (jack_port_t *) node->data;
		buf = jack_port_get_buffer (port, nframes);
		memset (buf, 0, sizeof (jack_default_audio_sample_t) * nframes);
	}
		
	if (driver->playback_handle) {
		if ((err = snd_pcm_drop (driver->playback_handle)) < 0) {
			jack_error ("ALSA: channel flush for playback "
				    "failed (%s)", snd_strerror (err));
			return -1;
		}
	}

	if (!driver->playback_handle
	    || driver->capture_and_playback_not_synced) {
		if (driver->capture_handle) {
			if ((err = snd_pcm_drop (driver->capture_handle)) < 0) {
				jack_error ("ALSA: channel flush for "
					    "capture failed (%s)",
					    snd_strerror (err));
				return -1;
			}
		}
	}
	
	if (driver->hw_monitoring) {
		driver->hw->set_input_monitor_mask (driver->hw, 0);
	}

	return 0;
}

static int
alsa_driver_restart (alsa_driver_t *driver)
{
 	if (driver->nt_stop((struct _jack_driver_nt *) driver))
 	    return -1;
 	return driver->nt_start((struct _jack_driver_nt *) driver);
}

static int
alsa_driver_xrun_recovery (alsa_driver_t *driver, float *delayed_usecs)
{
	snd_pcm_status_t *status;
	int res;

	snd_pcm_status_alloca(&status);

	if (driver->capture_handle) {
		if ((res = snd_pcm_status(driver->capture_handle, status))
		    < 0) {
			jack_error("status error: %s", snd_strerror(res));
		}
	} else {
		if ((res = snd_pcm_status(driver->playback_handle, status))
		    < 0) {
			jack_error("status error: %s", snd_strerror(res));
		}
	}

	if (snd_pcm_status_get_state(status) == SND_PCM_STATE_XRUN
	    && driver->process_count > XRUN_REPORT_DELAY) {
		struct timeval now, diff, tstamp;
		driver->xrun_count++;
		gettimeofday(&now, 0);
		snd_pcm_status_get_trigger_tstamp(status, &tstamp);
		timersub(&now, &tstamp, &diff);
		*delayed_usecs = diff.tv_sec * 1000000.0 + diff.tv_usec;
		MESSAGE("\n\n**** alsa_pcm: xrun of at least %.3f "
			"msecs\n\n",
			*delayed_usecs / 1000.0);
	}

	if (alsa_driver_restart (driver)) {
		return -1;
	}
	return 0;
}	

void
alsa_driver_silence_untouched_channels (alsa_driver_t *driver,
					jack_nframes_t nframes)
{
	channel_t chn;
	jack_nframes_t buffer_frames = driver->frames_per_cycle * driver->playback_nperiods;

	for (chn = 0; chn < driver->playback_nchannels; chn++) {
		if ((driver->channels_not_done & (1<<chn))) { 
			if (driver->silent[chn] < buffer_frames) {
				alsa_driver_silence_on_channel_no_mark (
					driver, chn, nframes);
				driver->silent[chn] += nframes;
			}
		}
	}
}

void 
alsa_driver_set_clock_sync_status (alsa_driver_t *driver, channel_t chn,
				   ClockSyncStatus status)
{
	driver->clock_sync_data[chn] = status;
	alsa_driver_clock_sync_notify (driver, chn, status);
}

static int under_gdb = FALSE;

static jack_nframes_t 
alsa_driver_wait (alsa_driver_t *driver, int extra_fd, int *status, float
		  *delayed_usecs)
{
	snd_pcm_sframes_t avail = 0;
	snd_pcm_sframes_t capture_avail = 0;
	snd_pcm_sframes_t playback_avail = 0;
	int xrun_detected = FALSE;
	int need_capture;
	int need_playback;
	unsigned int i;
	jack_time_t poll_enter;
	jack_time_t poll_ret = 0;

	*status = -1;
	*delayed_usecs = 0;

	need_capture = driver->capture_handle ? 1 : 0;

	if (extra_fd >= 0) {
		need_playback = 0;
	} else {
		need_playback = driver->playback_handle ? 1 : 0;
	}

  again:
	
	while (need_playback || need_capture) {

		unsigned int p_timed_out, c_timed_out;
		unsigned int ci = 0;
		unsigned int nfds;

		nfds = 0;

		if (need_playback) {
			snd_pcm_poll_descriptors (driver->playback_handle,
						  &driver->pfd[0],
						  driver->playback_nfds);
			nfds += driver->playback_nfds;
		}
		
		if (need_capture) {
			snd_pcm_poll_descriptors (driver->capture_handle,
						  &driver->pfd[nfds],
						  driver->capture_nfds);
			ci = nfds;
			nfds += driver->capture_nfds;
		}

		/* ALSA doesn't set POLLERR in some versions of 0.9.X */
		
		for (i = 0; i < nfds; i++) {
			driver->pfd[i].events |= POLLERR;
		}

		if (extra_fd >= 0) {
			driver->pfd[nfds].fd = extra_fd;
			driver->pfd[nfds].events =
				POLLIN|POLLERR|POLLHUP|POLLNVAL;
			nfds++;
		}

		poll_enter = jack_get_microseconds ();

		if (poll_enter > driver->poll_next) {
			/*
			 * This processing cycle was delayed past the
			 * next due interrupt!  Do not account this as
			 * a wakeup delay:
			 */
			driver->poll_next = 0;
			driver->poll_late++;
		}

		if (poll (driver->pfd, nfds, driver->poll_timeout) < 0) {

			if (errno == EINTR) {
				printf ("poll interrupt\n");
				// this happens mostly when run
				// under gdb, or when exiting due to a signal
				if (under_gdb) {
					goto again;
				}
				*status = -2;
				return 0;
			}
			
			jack_error ("ALSA: poll call failed (%s)",
				    strerror (errno));
			*status = -3;
			return 0;
			
		}

		poll_ret = jack_get_microseconds ();

		if (extra_fd < 0) {
			if (driver->poll_next && poll_ret > driver->poll_next) {
				*delayed_usecs = poll_ret - driver->poll_next;
			} 
			driver->poll_last = poll_ret;
			driver->poll_next = poll_ret + driver->period_usecs;
			driver->engine->transport_cycle_start (driver->engine, 
							       poll_ret);
		}

#ifdef DEBUG_WAKEUP
		fprintf (stderr, "%" PRIu64 ": checked %d fds, %" PRIu64
			 " usecs since poll entered\n", poll_ret, nfds,
			 poll_ret - poll_enter);
#endif

		/* check to see if it was the extra FD that caused us
		 * to return from poll */

		if (extra_fd >= 0) {

			if (driver->pfd[nfds-1].revents == 0) {
				/* we timed out on the extra fd */

				*status = -4;
				return -1;
			} 

			/* if POLLIN was the only bit set, we're OK */

			*status = 0;
			return (driver->pfd[nfds-1].revents == POLLIN) ? 0 : -1;
		}

		p_timed_out = 0;
		
		if (need_playback) {
			for (i = 0; i < driver->playback_nfds; i++) {
				if (driver->pfd[i].revents & POLLERR) {
					xrun_detected = TRUE;
				}
				
				if (driver->pfd[i].revents == 0) {
					p_timed_out++;
#ifdef DEBUG_WAKEUP
					fprintf (stderr, "%" PRIu64
						 " playback stream timed out\n",
						 poll_ret);
#endif
				}
			}
			
			if (p_timed_out == 0) {
				need_playback = 0;
#ifdef DEBUG_WAKEUP
				fprintf (stderr, "%" PRIu64
					 " playback stream ready\n",
					 poll_ret);
#endif
			}
		}
		
		c_timed_out = 0;

		if (need_capture) {
			for (i = ci; i < nfds; i++) {
				if (driver->pfd[i].revents & POLLERR) {
					xrun_detected = TRUE;
				}
				
				if (driver->pfd[i].revents == 0) {
					c_timed_out++;
#ifdef DEBUG_WAKEUP
					fprintf (stderr, "%" PRIu64
						 " capture stream timed out\n",
						 poll_ret);
#endif
				}
			}
			
			if (c_timed_out == 0) {
				need_capture = 0;
#ifdef DEBUG_WAKEUP
				fprintf (stderr, "%" PRIu64
					 " capture stream ready\n",
					 poll_ret);
#endif
			}
		}
		
		if ((p_timed_out && (p_timed_out == driver->playback_nfds)) &&
		    (c_timed_out && (c_timed_out == driver->capture_nfds))){
			jack_error ("ALSA: poll time out, polled for %" PRIu64
				    " usecs",
				    poll_ret - poll_enter);
			*status = -5;
			return 0;
		}		

	}

	if (driver->capture_handle) {
		if ((capture_avail = snd_pcm_avail_update (
			     driver->capture_handle)) < 0) {
			if (capture_avail == -EPIPE) {
				xrun_detected = TRUE;
			} else {
				jack_error ("unknown ALSA avail_update return"
					    " value (%u)", capture_avail);
			}
		}
	} else {
		/* odd, but see min() computation below */
		capture_avail = INT_MAX; 
	}

	if (driver->playback_handle) {
		if ((playback_avail = snd_pcm_avail_update (
			     driver->playback_handle)) < 0) {
			if (playback_avail == -EPIPE) {
				xrun_detected = TRUE;
			} else {
				jack_error ("unknown ALSA avail_update return"
					    " value (%u)", playback_avail);
			}
		}
	} else {
		/* odd, but see min() computation below */
		playback_avail = INT_MAX; 
	}

	if (xrun_detected) {
		*status = alsa_driver_xrun_recovery (driver, delayed_usecs);
		return 0;
	}

	*status = 0;
	driver->last_wait_ust = poll_ret;

	avail = capture_avail < playback_avail ? capture_avail : playback_avail;

#ifdef DEBUG_WAKEUP
	fprintf (stderr, "wakeup complete, avail = %lu, pavail = %lu "
		 "cavail = %lu\n",
		 avail, playback_avail, capture_avail);
#endif

	/* mark all channels not done for now. read/write will change this */

	driver->channels_not_done = driver->channel_done_bits;

	/* constrain the available count to the nearest (round down) number of
	   periods.
	*/

	return avail - (avail % driver->frames_per_cycle);
}

static int
alsa_driver_null_cycle (alsa_driver_t* driver, jack_nframes_t nframes)
{
	jack_nframes_t nf;
	snd_pcm_uframes_t offset;
	snd_pcm_uframes_t contiguous;
	int chn;

	if (driver->capture_handle) {
		nf = nframes;
		offset = 0;
		while (nf) {
			
			contiguous = (nf > driver->frames_per_cycle) ?
				driver->frames_per_cycle : nf;
			
			if (snd_pcm_mmap_begin (
				    driver->capture_handle,
				    &driver->capture_areas,
				    (snd_pcm_uframes_t *) &offset, 
				    (snd_pcm_uframes_t *) &contiguous)) {
				return -1;
			}
		
			if (snd_pcm_mmap_commit (driver->capture_handle,
						 offset, contiguous) < 0) {
				return -1;
			}

			nf -= contiguous;
		}
	}

	if (driver->playback_handle) {
		nf = nframes;
		offset = 0;
		while (nf) {
			contiguous = (nf > driver->frames_per_cycle) ?
				driver->frames_per_cycle : nf;
			
			if (snd_pcm_mmap_begin (
				    driver->playback_handle,
				    &driver->playback_areas,
				    (snd_pcm_uframes_t *) &offset, 
				    (snd_pcm_uframes_t *) &contiguous)) {
				return -1;
			}
			
			for (chn = 0; chn < driver->playback_nchannels; chn++) {
				alsa_driver_silence_on_channel (driver, chn,
								contiguous);
			}
		
			if (snd_pcm_mmap_commit (driver->playback_handle,
						 offset, contiguous) < 0) {
				return -1;
			}

			nf -= contiguous;
		}
	}

	return 0;
}

static int
alsa_driver_bufsize (alsa_driver_t* driver, jack_nframes_t nframes)
{
	return alsa_driver_reset_parameters (driver, nframes,
					     driver->user_nperiods,
					     driver->frame_rate);
}

static int
alsa_driver_read (alsa_driver_t *driver, jack_nframes_t nframes)
{
	snd_pcm_sframes_t contiguous;
	snd_pcm_sframes_t nread;
	snd_pcm_sframes_t offset;
	jack_default_audio_sample_t* buf;
	channel_t chn;
	JSList *node;
	jack_port_t* port;
	int err;

	if (!driver->capture_handle || driver->engine->freewheeling) {
		return 0;
	}

	nread = 0;
	contiguous = 0;

	while (nframes) {
		
		contiguous = (nframes > driver->frames_per_cycle) ?
			driver->frames_per_cycle : nframes;
		
		if (alsa_driver_get_channel_addresses (
			    driver, 
			    (snd_pcm_uframes_t *) &contiguous, 
			    (snd_pcm_uframes_t *) 0,
			    &offset, 0) < 0) {
			return -1;
		}
			
		for (chn = 0, node = driver->capture_ports; node;
		     node = jack_slist_next (node), chn++) {
			
			port = (jack_port_t *) node->data;
			
			if (!jack_port_connected (port)) {
				/* no-copy optimization */
				continue;
			}
			buf = jack_port_get_buffer (port, nframes);
			alsa_driver_read_from_channel (driver, chn,
				buf + nread, contiguous);
		}
		
		if ((err = snd_pcm_mmap_commit (driver->capture_handle,
				offset, contiguous)) < 0) {
			jack_error ("ALSA: could not complete read of %"
				PRIu32 " frames: error = %d\n", contiguous, err);
			return -1;
		}

		nframes -= contiguous;
	}

	return 0;
}

static int
alsa_driver_write (alsa_driver_t* driver, jack_nframes_t nframes)
{
	channel_t chn;
	JSList *node;
	jack_default_audio_sample_t* buf;
	snd_pcm_sframes_t nwritten;
	snd_pcm_sframes_t contiguous;
	snd_pcm_sframes_t offset;
	jack_port_t *port;
	int err;

	driver->process_count++;

	if (!driver->playback_handle || driver->engine->freewheeling) {
		return 0;
	}

	nwritten = 0;
	contiguous = 0;
	
	/* check current input monitor request status */
	
	driver->input_monitor_mask = 0;
	
	for (chn = 0, node = driver->capture_ports; node;
	     node = jack_slist_next (node), chn++) {
		if (((jack_port_t *) node->data)->shared->monitor_requests) {
			driver->input_monitor_mask |= (1<<chn);
		}
	}

	if (driver->hw_monitoring) {
		if ((driver->hw->input_monitor_mask
		     != driver->input_monitor_mask)
		    && !driver->all_monitor_in) {
			driver->hw->set_input_monitor_mask (
				driver->hw, driver->input_monitor_mask);
		}
	}
	
	while (nframes) {
		
		contiguous = (nframes > driver->frames_per_cycle) ?
			driver->frames_per_cycle : nframes;
		
		if (alsa_driver_get_channel_addresses (
			    driver, 
			    (snd_pcm_uframes_t *) 0,
			    (snd_pcm_uframes_t *) &contiguous, 
			    0, &offset) < 0) {
			return -1;
		}
		
		for (chn = 0, node = driver->playback_ports; node;
		     node = jack_slist_next (node), chn++) {

			port = (jack_port_t *) node->data;

			if (!jack_port_connected (port)) {
				continue;
			}
			buf = jack_port_get_buffer (port, contiguous);
			alsa_driver_write_to_channel (driver, chn,
				buf + nwritten, contiguous);
		}

		if (driver->channels_not_done) {
			alsa_driver_silence_untouched_channels (driver,
								contiguous);
		}
		
		if ((err = snd_pcm_mmap_commit (driver->playback_handle,
				offset, contiguous)) < 0) {
			jack_error ("ALSA: could not complete playback of %"
				PRIu32 " frames: error = %d", contiguous, err);
			if (err != EPIPE && err != ESTRPIPE)
				return -1;
		}

		nframes -= contiguous;
	}

	return 0;
}

static inline int
alsa_driver_run_cycle (alsa_driver_t *driver)
{
	jack_engine_t *engine = driver->engine;
	int wait_status;
	float delayed_usecs;
	jack_nframes_t nframes;

	DEBUG ("alsa run cycle wait\n");

	nframes = alsa_driver_wait (driver, -1, &wait_status, &delayed_usecs);

	DEBUG ("alsa back from wait, nframes = %lu", nframes);

	if (nframes == 0) {

		/* we detected an xrun and restarted: notify
		 * clients about the delay. 
		 */
		engine->delay (engine, delayed_usecs);
		return 0;
	} 

	if (wait_status == 0)
		return engine->run_cycle (engine, nframes, delayed_usecs);

	if (wait_status < 0)
		return -1;		/* driver failed */
	else
		return 0;
}

static int
alsa_driver_attach (alsa_driver_t *driver)
{
	char buf[32];
	channel_t chn;
	jack_port_t *port;
	int port_flags;

	driver->engine->set_buffer_size (driver->engine, driver->frames_per_cycle);
	driver->engine->set_sample_rate (driver->engine, driver->frame_rate);

	port_flags = JackPortIsOutput|JackPortIsPhysical|JackPortIsTerminal;

	if (driver->has_hw_monitoring) {
		port_flags |= JackPortCanMonitor;
	}

	for (chn = 0; chn < driver->capture_nchannels; chn++) {

		snprintf (buf, sizeof(buf) - 1, "capture_%lu", chn+1);

		if ((port = jack_port_register (driver->client, buf,
						JACK_DEFAULT_AUDIO_TYPE,
						port_flags, 0)) == NULL) {
			jack_error ("ALSA: cannot register port for %s", buf);
			break;
		}

		/* XXX fix this so that it can handle: systemic
		 * (external) latency
		*/
		jack_port_set_latency (port, driver->frames_per_cycle);

		driver->capture_ports =
			jack_slist_append (driver->capture_ports, port);
	}
	
	port_flags = JackPortIsInput|JackPortIsPhysical|JackPortIsTerminal;

	for (chn = 0; chn < driver->playback_nchannels; chn++) {
		jack_port_t *monitor_port;

		snprintf (buf, sizeof(buf) - 1, "playback_%lu", chn+1);

		if ((port = jack_port_register (driver->client, buf,
						JACK_DEFAULT_AUDIO_TYPE,
						port_flags, 0)) == NULL) {
			jack_error ("ALSA: cannot register port for %s", buf);
			break;
		}
		
		/* XXX fix this so that it can handle: systemic
		 * (external) latency
		*/
		jack_port_set_latency (port, driver->frames_per_cycle
				       * driver->user_nperiods);

		driver->playback_ports =
			jack_slist_append (driver->playback_ports, port);

		if (driver->with_monitor_ports) {
			snprintf (buf, sizeof(buf) - 1, "monitor_%lu", chn+1);
			
			if ((monitor_port = jack_port_register (
				     driver->client, buf,
				     JACK_DEFAULT_AUDIO_TYPE,
				     JackPortIsOutput, 0)) == NULL) {
				jack_error ("ALSA: cannot register monitor "
					    "port for %s", buf);
			} else {
				jack_port_tie (port, monitor_port);
			}
		}
	}


	return jack_activate (driver->client);
}

static int
alsa_driver_detach (alsa_driver_t *driver)
{
	JSList *node;

	if (driver->engine == NULL) {
		return 0;
	}

	for (node = driver->capture_ports; node;
	     node = jack_slist_next (node)) {
		jack_port_unregister (driver->client,
				      ((jack_port_t *) node->data));
	}

	jack_slist_free (driver->capture_ports);
	driver->capture_ports = 0;
		
	for (node = driver->playback_ports; node;
	     node = jack_slist_next (node)) {
		jack_port_unregister (driver->client,
				      ((jack_port_t *) node->data));
	}

	jack_slist_free (driver->playback_ports);
	driver->playback_ports = 0;

	return 0;
}

#if 0
static int  /* UNUSED */
alsa_driver_change_sample_clock (alsa_driver_t *driver, SampleClockMode mode)

{
	return driver->hw->change_sample_clock (driver->hw, mode);
}

static void  /* UNUSED */
alsa_driver_request_all_monitor_input (alsa_driver_t *driver, int yn)

{
	if (driver->hw_monitoring) {
		if (yn) {
			driver->hw->set_input_monitor_mask (driver->hw, ~0U);
		} else {
			driver->hw->set_input_monitor_mask (
				driver->hw, driver->input_monitor_mask);
		}
	}

	driver->all_monitor_in = yn;
}

static void  /* UNUSED */
alsa_driver_set_hw_monitoring (alsa_driver_t *driver, int yn)
{
	if (yn) {
		driver->hw_monitoring = TRUE;
		
		if (driver->all_monitor_in) {
			driver->hw->set_input_monitor_mask (driver->hw, ~0U);
		} else {
			driver->hw->set_input_monitor_mask (
				driver->hw, driver->input_monitor_mask);
		}
	} else {
		driver->hw_monitoring = FALSE;
		driver->hw->set_input_monitor_mask (driver->hw, 0);
	}
}

static ClockSyncStatus  /* UNUSED */
alsa_driver_clock_sync_status (channel_t chn)
{
	return Lock;
}
#endif

static void
alsa_driver_delete (alsa_driver_t *driver)
{
	JSList *node;

	for (node = driver->clock_sync_listeners; node;
	     node = jack_slist_next (node)) {
		free (node->data);
	}
	jack_slist_free (driver->clock_sync_listeners);

	if (driver->capture_handle) {
		snd_pcm_close (driver->capture_handle);
		driver->capture_handle = 0;
	} 

	if (driver->playback_handle) {
		snd_pcm_close (driver->playback_handle);
		driver->capture_handle = 0;
	}
	
	if (driver->capture_hw_params) {
		snd_pcm_hw_params_free (driver->capture_hw_params);
		driver->capture_hw_params = 0;
	}

	if (driver->playback_hw_params) {
		snd_pcm_hw_params_free (driver->playback_hw_params);
		driver->playback_hw_params = 0;
	}
	
	if (driver->capture_sw_params) {
		snd_pcm_sw_params_free (driver->capture_sw_params);
		driver->capture_sw_params = 0;
	}
	
	if (driver->playback_sw_params) {
		snd_pcm_sw_params_free (driver->playback_sw_params);
		driver->playback_sw_params = 0;
	}

	if (driver->pfd) {
		free (driver->pfd);
	}
	
	if (driver->hw) {
		driver->hw->release (driver->hw);
		driver->hw = 0;
	}
	free(driver->alsa_name_playback);
	free(driver->alsa_name_capture);
	free(driver->alsa_driver);

	alsa_driver_release_channel_dependent_memory (driver);
        jack_driver_nt_finish ((jack_driver_nt_t *) driver);
	free (driver);
}

static jack_driver_t *
alsa_driver_new (char *name, char *playback_alsa_device,
		 char *capture_alsa_device,
		 jack_client_t *client, 
		 jack_nframes_t frames_per_cycle,
		 jack_nframes_t user_nperiods,
		 jack_nframes_t rate,
		 int hw_monitoring,
		 int hw_metering,
		 int capturing,
		 int playing,
		 DitherAlgorithm dither,
		 int soft_mode, 
		 int monitor,
		 int user_capture_nchnls,
		 int user_playback_nchnls,
		 int shorts_first
		 )
{
	int err;

	alsa_driver_t *driver;

	printf ("creating alsa driver ... %s|%s|%" PRIu32 "|%" PRIu32
		"|%" PRIu32"|%" PRIu32"|%" PRIu32 "|%s|%s|%s|%s\n",
		playing ? playback_alsa_device : "-",
		capturing ? capture_alsa_device : "-", 
		frames_per_cycle, user_nperiods, rate,
		user_capture_nchnls,user_playback_nchnls,
		hw_monitoring ? "hwmon": "nomon",
		hw_metering ? "hwmeter":"swmeter",
		soft_mode ? "soft-mode":"-",
		shorts_first ? "16bit":"32bit");

	driver = (alsa_driver_t *) calloc (1, sizeof (alsa_driver_t));

	jack_driver_nt_init ((jack_driver_nt_t *) driver);

	driver->nt_attach = (JackDriverNTAttachFunction) alsa_driver_attach;
        driver->nt_detach = (JackDriverNTDetachFunction) alsa_driver_detach;
	driver->read = (JackDriverReadFunction) alsa_driver_read;
	driver->write = (JackDriverReadFunction) alsa_driver_write;
	driver->null_cycle =
		(JackDriverNullCycleFunction) alsa_driver_null_cycle;
	driver->nt_bufsize = (JackDriverNTBufSizeFunction) alsa_driver_bufsize;
	driver->nt_start = (JackDriverNTStartFunction) alsa_driver_start;
	driver->nt_stop = (JackDriverNTStopFunction) alsa_driver_stop;
	driver->nt_run_cycle = (JackDriverNTRunCycleFunction) alsa_driver_run_cycle;

	driver->playback_handle = NULL;
	driver->capture_handle = NULL;
	driver->ctl_handle = 0;
	driver->hw = 0;
	driver->capture_and_playback_not_synced = FALSE;
	driver->max_nchannels = 0;
	driver->user_nchannels = 0;
	driver->playback_nchannels = user_playback_nchnls;
	driver->capture_nchannels = user_capture_nchnls;
	driver->playback_sample_bytes = (shorts_first ? 2:4);
	driver->capture_sample_bytes = (shorts_first ? 2:4);

	driver->playback_addr = 0;
	driver->capture_addr = 0;


	driver->silent = 0;
	driver->all_monitor_in = FALSE;
	driver->with_monitor_ports = monitor;

	driver->clock_mode = ClockMaster; /* XXX is it? */
	driver->input_monitor_mask = 0;   /* XXX is it? */

	driver->capture_ports = 0;
	driver->playback_ports = 0;

	driver->pfd = 0;
	driver->playback_nfds = 0;
	driver->capture_nfds = 0;

	driver->dither = dither;
	driver->soft_mode = soft_mode;

	pthread_mutex_init (&driver->clock_sync_lock, 0);
	driver->clock_sync_listeners = 0;

	driver->poll_late = 0;
	driver->xrun_count = 0;
	driver->process_count = 0;

	driver->alsa_name_playback = strdup (playback_alsa_device);
	driver->alsa_name_capture = strdup (capture_alsa_device);

	if (alsa_driver_check_card_type (driver)) {
		alsa_driver_delete (driver);
		return NULL;
	}

	alsa_driver_hw_specific (driver, hw_monitoring, hw_metering);

	if (playing) {
		if (snd_pcm_open (&driver->playback_handle,
				  playback_alsa_device,
				  SND_PCM_STREAM_PLAYBACK,
				  SND_PCM_NONBLOCK) < 0) {
			switch (errno) {
			case EBUSY:
				jack_error ("the playback device \"%s\" is "
					    "already in use. Please stop the"
					    " application using it and "
					    "run JACK again",
					    playback_alsa_device);
				alsa_driver_delete (driver);
				return NULL;
				break;

			case EPERM:
				jack_error ("you do not have permission to open "
					    "the audio device \"%s\" for playback",
					    playback_alsa_device);
				alsa_driver_delete (driver);
				return NULL;
				break;
			} 
			
			driver->playback_handle = NULL;
		} 

		if (driver->playback_handle) {
			snd_pcm_nonblock (driver->playback_handle, 0);
		}
	} 

	if (capturing) {
		if (snd_pcm_open (&driver->capture_handle,
				  capture_alsa_device,
				  SND_PCM_STREAM_CAPTURE,
				  SND_PCM_NONBLOCK) < 0) {
			switch (errno) {
			case EBUSY:
				jack_error ("the capture device \"%s\" is "
					    "already in use. Please stop the"
					    " application using it and "
					    "run JACK again",
					    capture_alsa_device);
				alsa_driver_delete (driver);
				return NULL;
				break;

			case EPERM:
				jack_error ("you do not have permission to open "
					    "the audio device \"%s\" for capture",
					    capture_alsa_device);
				alsa_driver_delete (driver);
				return NULL;
				break;
			} 

			driver->capture_handle = NULL;
		}

		if (driver->capture_handle) {
			snd_pcm_nonblock (driver->capture_handle, 0);
		}
	}

	if (driver->playback_handle == NULL) {
		if (playing) {

			/* they asked for playback, but we can't do it */

			jack_error ("ALSA: Cannot open PCM device %s for "
				    "playback. Falling back to capture-only"
				    " mode", name);

			if (driver->capture_handle == NULL) {
				/* can't do anything */
				alsa_driver_delete (driver);
				return NULL;
			}
			
			playing = FALSE;
		}
	}

	if (driver->capture_handle == NULL) {
		if (capturing) {

			/* they asked for capture, but we can't do it */
			
			jack_error ("ALSA: Cannot open PCM device %s for "
				    "capture. Falling back to playback-only"
				    " mode", name);
			
			if (driver->playback_handle == NULL) {
				/* can't do anything */
				alsa_driver_delete (driver);
				return NULL;
			}

			capturing = FALSE;
		}
	}

	driver->playback_hw_params = 0;
	driver->capture_hw_params = 0;
	driver->playback_sw_params = 0;
	driver->capture_sw_params = 0;

	if (driver->playback_handle) {
		if ((err = snd_pcm_hw_params_malloc (
			     &driver->playback_hw_params)) < 0) {
			jack_error ("ALSA: could not allocate playback hw"
				    " params structure");
			alsa_driver_delete (driver);
			return NULL;
		}

		if ((err = snd_pcm_sw_params_malloc (
			     &driver->playback_sw_params)) < 0) {
			jack_error ("ALSA: could not allocate playback sw"
				    " params structure");
			alsa_driver_delete (driver);
			return NULL;
		}
	}

	if (driver->capture_handle) {
		if ((err = snd_pcm_hw_params_malloc (
			     &driver->capture_hw_params)) < 0) {
			jack_error ("ALSA: could not allocate capture hw"
				    " params structure");
			alsa_driver_delete (driver);
			return NULL;
		}

		if ((err = snd_pcm_sw_params_malloc (
			     &driver->capture_sw_params)) < 0) {
			jack_error ("ALSA: could not allocate capture sw"
				    " params structure");
			alsa_driver_delete (driver);
			return NULL;
		}
	}

	if (alsa_driver_set_parameters (driver, frames_per_cycle,
					user_nperiods, rate)) {
		alsa_driver_delete (driver);
		return NULL;
	}

	driver->capture_and_playback_not_synced = FALSE;

	if (driver->capture_handle && driver->playback_handle) {
		if (snd_pcm_link (driver->capture_handle,
				  driver->playback_handle) != 0) {
			driver->capture_and_playback_not_synced = TRUE;
		} 
	}

	driver->client = client;

	return (jack_driver_t *) driver;
}

int
alsa_driver_listen_for_clock_sync_status (alsa_driver_t *driver, 
					  ClockSyncListenerFunction func,
					  void *arg)
{
	ClockSyncListener *csl;

	csl = (ClockSyncListener *) malloc (sizeof (ClockSyncListener));
	csl->function = func;
	csl->arg = arg;
	csl->id = driver->next_clock_sync_listener_id++;
	
	pthread_mutex_lock (&driver->clock_sync_lock);
	driver->clock_sync_listeners =
		jack_slist_prepend (driver->clock_sync_listeners, csl);
	pthread_mutex_unlock (&driver->clock_sync_lock);
	return csl->id;
}

int
alsa_driver_stop_listening_to_clock_sync_status (alsa_driver_t *driver,
						 unsigned int which)

{
	JSList *node;
	int ret = -1;
	pthread_mutex_lock (&driver->clock_sync_lock);
	for (node = driver->clock_sync_listeners; node;
	     node = jack_slist_next (node)) {
		if (((ClockSyncListener *) node->data)->id == which) {
			driver->clock_sync_listeners =
				jack_slist_remove_link (
					driver->clock_sync_listeners, node);
			free (node->data);
			jack_slist_free_1 (node);
			ret = 0;
			break;
		}
	}
	pthread_mutex_unlock (&driver->clock_sync_lock);
	return ret;
}

void 
alsa_driver_clock_sync_notify (alsa_driver_t *driver, channel_t chn,
			       ClockSyncStatus status)
{
	JSList *node;

	pthread_mutex_lock (&driver->clock_sync_lock);
	for (node = driver->clock_sync_listeners; node;
	     node = jack_slist_next (node)) {
		ClockSyncListener *csl = (ClockSyncListener *) node->data;
		csl->function (chn, status, csl->arg);
	}
	pthread_mutex_unlock (&driver->clock_sync_lock);

}

static int
dither_opt (char c, DitherAlgorithm* dither)
{
	switch (c) {
	case '-':
	case 'n':
		*dither = None;
		break;
		
	case 'r':
		*dither = Rectangular;
		break;
		
	case 's':
		*dither = Shaped;
		break;
		
	case 't':
		*dither = Triangular;
		break;
		
	default:
		fprintf (stderr, "ALSA driver: illegal dithering mode %c\n", c);
		return -1;
	}
	return 0;
}


/* DRIVER "PLUGIN" INTERFACE */

const char driver_client_name[] = "alsa_pcm";

const jack_driver_desc_t *
driver_get_descriptor ()
{
	jack_driver_desc_t * desc;
	jack_driver_param_desc_t * params;
	unsigned int i;

	desc = calloc (1, sizeof (jack_driver_desc_t));

	strcpy (desc->name,"alsa");
	desc->nparams = 15;
  
	params = calloc (desc->nparams, sizeof (jack_driver_param_desc_t));

	i = 0;
	strcpy (params[i].name, "capture");
	params[i].character  = 'C';
	params[i].type       = JackDriverParamString;
	strcpy (params[i].value.str, "none");
	strcpy (params[i].short_desc,
		"Provide capture ports.  Optionally set device");
	strcpy (params[i].long_desc, params[i].short_desc);

	i++;
	strcpy (params[i].name, "playback");
	params[i].character  = 'P';
	params[i].type       = JackDriverParamString;
	strcpy (params[i].value.str, "none");
	strcpy (params[i].short_desc,
		"Provide playback ports.  Optionally set device");
	strcpy (params[i].long_desc, params[i].short_desc);

	i++;
	strcpy (params[i].name, "device");
	params[i].character  = 'd';
	params[i].type       = JackDriverParamString;
	strcpy (params[i].value.str,  "hw:0");
	strcpy (params[i].short_desc, "ALSA device name");
	strcpy (params[i].long_desc,  params[i].short_desc);

	i++;
	strcpy (params[i].name, "rate");
	params[i].character  = 'r';
	params[i].type       = JackDriverParamUInt;
	params[i].value.ui   = 48000U;
	strcpy (params[i].short_desc, "Sample rate");
	strcpy (params[i].long_desc, params[i].short_desc);

	i++;
	strcpy (params[i].name, "period");
	params[i].character  = 'p';
	params[i].type       = JackDriverParamUInt;
	params[i].value.ui   = 1024U;
	strcpy (params[i].short_desc, "Frames per period");
	strcpy (params[i].long_desc, params[i].short_desc);

	i++;
	strcpy (params[i].name, "nperiods");
	params[i].character  = 'n';
	params[i].type       = JackDriverParamUInt;
	params[i].value.ui   = 2U;
	strcpy (params[i].short_desc, "Number of periods of playback latency");
	strcpy (params[i].long_desc, params[i].short_desc);

	i++;
	strcpy (params[i].name, "hwmon");
	params[i].character  = 'H';
	params[i].type       = JackDriverParamBool;
	params[i].value.i    = 0;
	strcpy (params[i].short_desc,"Hardware monitoring, if available");
	strcpy (params[i].long_desc, params[i].short_desc);

	i++;
	strcpy (params[i].name, "hwmeter");
	params[i].character  = 'M';
	params[i].type       = JackDriverParamBool;
	params[i].value.i    = 0;
	strcpy (params[i].short_desc, "Hardware metering, if available");
	strcpy (params[i].long_desc, params[i].short_desc);

	i++;
	strcpy (params[i].name, "duplex");
	params[i].character  = 'D';
	params[i].type       = JackDriverParamBool;
	params[i].value.i    = 1;
	strcpy (params[i].short_desc,
		"Provide both capture and playback ports");
	strcpy (params[i].long_desc, params[i].short_desc);

	i++;
	strcpy (params[i].name, "softmode");
	params[i].character  = 's';
 	params[i].type       = JackDriverParamBool;
	params[i].value.i    = 0;
	strcpy (params[i].short_desc, "Soft-mode, no xrun handling");
	strcpy (params[i].long_desc,  params[i].short_desc);

	i++;
	strcpy (params[i].name, "monitor");
	params[i].character  = 'm';
	params[i].type       = JackDriverParamBool;
	params[i].value.i    = 0;
	strcpy (params[i].short_desc, "Provide monitor ports for the output");
	strcpy (params[i].long_desc, params[i].short_desc);

	i++;
	strcpy (params[i].name, "dither");
	params[i].character  = 'z';
	params[i].type       = JackDriverParamChar;
	params[i].value.c    = 'n';
	strcpy (params[i].short_desc, "Dithering mode");
	strcpy (params[i].long_desc,
		"Dithering mode:\n"
		"  n - none\n"
		"  r - rectangular\n"
		"  s - shaped\n"
		"  t - triangular");

	i++;
	strcpy (params[i].name, "inchannels");
	params[i].character  = 'i';
	params[i].type       = JackDriverParamUInt;
	params[i].value.i    = 0;
	strcpy (params[i].short_desc,
		"Number of capture channels (defaults to hardware max)");
	strcpy (params[i].long_desc, params[i].short_desc);

	i++;
	strcpy (params[i].name, "outchannels");
	params[i].character  = 'o';
	params[i].type       = JackDriverParamUInt;
	params[i].value.i    = 0;
	strcpy (params[i].short_desc,
		"Number of playback channels (defaults to hardware max)");
	strcpy (params[i].long_desc, params[i].short_desc);

	i++;
	strcpy (params[i].name, "shorts");
	params[i].character  = 'S';
	params[i].type       = JackDriverParamBool;
	params[i].value.i    = FALSE;
	strcpy (params[i].short_desc, "Try 16-bit samples before 32-bit");
	strcpy (params[i].long_desc, params[i].short_desc);

	desc->params = params;

	return desc;
}

jack_driver_t *
driver_initialize (jack_client_t *client, const JSList * params)
{
        jack_nframes_t srate = 48000;
	jack_nframes_t frames_per_interrupt = 1024;
	unsigned long user_nperiods = 2;
	char *playback_pcm_name = "hw:0";
	char *capture_pcm_name = "hw:0";
	int hw_monitoring = FALSE;
	int hw_metering = FALSE;
	int capture = FALSE;
	int playback = FALSE;
	int soft_mode = FALSE;
	int monitor = FALSE;
	DitherAlgorithm dither = None;
	int user_capture_nchnls = 0;
	int user_playback_nchnls = 0;
	int shorts_first = FALSE;
	const JSList * node;
	const jack_driver_param_t * param;

	for (node = params; node; node = jack_slist_next (node)) {
  	        param = (const jack_driver_param_t *) node->data;

		switch (param->character) {

		case 'C':
			capture = TRUE;
			if (strcmp (param->value.str, "none") != 0) {
				capture_pcm_name = strdup (param->value.str);
			}
			break;

		case 'P':
			playback = TRUE;
			if (strcmp (param->value.str, "none") != 0) {
				playback_pcm_name = strdup (param->value.str);
			}
			break;

		case 'D':
			playback = TRUE;
			capture = TRUE;
			break;

		case 'd':
			playback_pcm_name = strdup (param->value.str);
			capture_pcm_name  = strdup (param->value.str);
			break;

		case 'H':
			hw_monitoring = param->value.i;
			break;

		case 'm':
			monitor = param->value.i;
			break;

		case 'M':
			hw_metering = param->value.i;
			break;

		case 'r':
		        srate = param->value.ui;
			fprintf (stderr, "apparent rate = %d\n", srate);
		        break;
			
		case 'p':
			frames_per_interrupt = param->value.ui;
			break;
				
		case 'n':
			user_nperiods = param->value.ui;
			break;
				
		case 's':
			soft_mode = param->value.i;
			break;

		case 'z':
			if (dither_opt (param->value.c, &dither)) {
			  return NULL;
			}
			break;

		case 'i':
			user_capture_nchnls = param->value.ui;
			break;
		case 'o':
		        user_playback_nchnls = param->value.ui;
			break;

		case 'S':
			shorts_first = param->value.i;
			break;

		}
	}
			
	/* duplex is the default */
	if (!capture && !playback) {
		capture = TRUE;
		playback = TRUE;
	}

	return alsa_driver_new ("alsa_pcm", playback_pcm_name,
				capture_pcm_name, client,
				frames_per_interrupt, 
				user_nperiods, srate, hw_monitoring,
				hw_metering, capture, playback, dither,
				soft_mode, monitor, 
				user_capture_nchnls, user_playback_nchnls,
				shorts_first);
}

void
driver_finish (jack_driver_t *driver)
{
	alsa_driver_delete ((alsa_driver_t *) driver);
}
