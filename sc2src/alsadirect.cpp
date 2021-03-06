/* Copyright (C) 2014 J.F.Dockes
 *       This program is free software; you can redistribute it and/or modify
 *       it under the terms of the GNU General Public License as published by
 *       the Free Software Foundation; either version 2 of the License, or
 *       (at your option) any later version.
 *
 *       This program is distributed in the hope that it will be useful,
 *       but WITHOUT ANY WARRANTY; without even the implied warranty of
 *       MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *       GNU General Public License for more details.
 *
 *       You should have received a copy of the GNU General Public License
 *       along with this program; if not, write to the
 *       Free Software Foundation, Inc.,
 *       59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.
 */
#include "config.h"

#include <string.h>
#include <sys/types.h>
#include <math.h>

#ifdef WORDS_BIGENDIAN
#if HAVE_BYTESWAP_H
#include <byteswap.h>
#define BSWAP16(X) bswap_16(X)
#else
#define BSWAP16(X) ((((X) & 0xff) >> 8) | ((X) << 8))
#endif
#else // Little endian -> 
#define BSWAP16(X) (X)
#endif

#include <iostream>
#include <queue>
#include <alsa/asoundlib.h>

#include <samplerate.h>

#include "log.h"
#include "rcvqueue.h"
#include "conftree.h"

using namespace std;

#ifndef MIN
#define MIN(A, B) ((A) < (B) ? (A) : (B))
#endif

// The queue for audio blocks ready for alsa. This is the maximum size
// before enqueuing blocks
static const unsigned int qs_hi = 100;

// Queue size target including alsa buffers. There is no particular
// reason for the qs_hi/2 value. We could try something lower to
// minimize latency
static const unsigned int qstarg = qs_hi/2;

static WorkQueue<AudioMessage*> alsaqueue("alsaqueue", qs_hi);

/* This is used to disable sample rate conversion until playing is actually 
   started */
static bool qinit = false;

static snd_pcm_t *pcm;

// A period is data processed between interrupts. When playing,
// there is one period belonging to the hardware and normally
// others that the software can fill up. The minimum reasonable is
// 2 periods (one for us, one for the hardware), which we try to
// use as this gives minimum latency while being workable. But we
// have to accept that the driver may have other constraints.  Not
// too sure why we bother actually, because we don't use the
// resulting config at all while writing...
// In any case, if we get what we ask for, we have an in-driver
// latency of between 16KBytes and 32KBytes, 4K to 8K frames (16:2),
// 200mS at 44.1 Khz
//
// These may be changed depending on local alsa caps:
static snd_pcm_uframes_t periodsize = 16384; /* Periodsize (bytes) */
static unsigned int periods = 2;       /* Number of periods */

static void *alsawriter(void *p)
{
    while (true) {
        if (!qinit) {
            if (!alsaqueue.waitminsz(qstarg)) {
                LOGERR("alsawriter: waitminsz failed\n");
                alsaqueue.workerExit();
                return (void *)1;
            }
        }
        AudioMessage *tsk = 0;
        size_t qsz;
        if (!alsaqueue.take(&tsk, &qsz)) {
            // TBD: reset alsa?
            alsaqueue.workerExit();
            return (void*)1;
        }
        // Bufs 
        snd_pcm_uframes_t frames = tsk->frames();
        snd_pcm_sframes_t ret =  snd_pcm_writei(pcm, tsk->m_buf, frames);
        if (ret != int(frames)) {
            LOGERR("snd-cm_writei(" << frames <<" frames) failed: ret: " <<
                   ret << endl);
            if (ret < 0) {
                qinit = false;
                snd_pcm_prepare(pcm);
            }
        } else {
            qinit = true;
        }
        delete tsk;
    }
}

static bool alsa_init(const string& dev, AudioMessage *tsk)
{
    snd_pcm_hw_params_t *hwparams;
    int err;
    const char *cmd = "";
    int dir=0;
    unsigned int actual_rate = tsk->m_freq;

    if ((err = snd_pcm_open(&pcm, dev.c_str(), 
                            SND_PCM_STREAM_PLAYBACK, 0)) < 0) {
        LOGERR("alsa_init: snd_pcm_open " << dev << " " << 
               snd_strerror(err) << endl);
        return false;;
    }
    if ((err = snd_pcm_hw_params_malloc(&hwparams)) < 0) {
        LOGERR("alsa_init: snd_pcm_hw_params_malloc " << 
               snd_strerror(err) << endl);
        snd_pcm_close(pcm);
        return false;
    }

    cmd = "snd_pcm_hw_params_any";
    if ((err = snd_pcm_hw_params_any(pcm, hwparams)) < 0) {
        goto error;
    }
    cmd = "snd_pcm_hw_params_set_access";
    if ((err = 
         snd_pcm_hw_params_set_access(pcm, hwparams, 
                                      SND_PCM_ACCESS_RW_INTERLEAVED)) < 0) {
        goto error;
    }

    cmd = "snd_pcm_hw_params_set_format";
    if ((err = 
         snd_pcm_hw_params_set_format(pcm, hwparams, 
                                      SND_PCM_FORMAT_S16_LE)) < 0) {
        goto error;
    }
    cmd = "snd_pcm_hw_params_set_channels";
    if ((err = snd_pcm_hw_params_set_channels(pcm, hwparams, 
                                              tsk->m_chans)) < 0) {
        goto error;
    }
    cmd = "snd_pcm_hw_params_set_rate_near";
    if ((err = snd_pcm_hw_params_set_rate_near(pcm, hwparams, 
                                               &actual_rate, &dir)) < 0) {
        goto error;
    }

    unsigned int periodsmin, periodsmax;
    snd_pcm_hw_params_get_periods_min(hwparams, &periodsmin, &dir);
    snd_pcm_hw_params_get_periods_max(hwparams, &periodsmax, &dir);
    LOGDEB("Alsa: periods min " << periodsmin << 
           " max " << periodsmax << endl);
    periods = 2;
    if (periods < periodsmin || periods > periodsmax)
        periods = periodsmin;
    cmd = "snd_pcm_hw_params_set_periods";
    if ((err = snd_pcm_hw_params_set_periods(pcm, hwparams, periods, 0)) < 0) {
        goto error;
    }
    snd_pcm_uframes_t bsmax, bsmin;
    snd_pcm_hw_params_get_buffer_size_min(hwparams, &bsmin);
    snd_pcm_hw_params_get_buffer_size_max(hwparams, &bsmax);
    unsigned int bufferframes;
    bufferframes = periodsize * periods / (2*tsk->m_chans);
    if (bufferframes < bsmin || bufferframes > bsmax) {
        bufferframes = bsmin;
        periodsize = bufferframes / periods * (2 * tsk->m_chans);
    }
    cmd = "snd_pcm_hw_params_set_buffer_size";
    LOGDEB("Alsa: set buffer_size: min " << bsmin << " max " << bsmax << 
           " val " << bufferframes << endl);
    if ((err = snd_pcm_hw_params_set_buffer_size(pcm, hwparams, bufferframes)) 
        < 0) {
        goto error;
    }
  
    cmd = "snd_pcm_hw_params";
    if ((err = snd_pcm_hw_params(pcm, hwparams)) < 0) {
        goto error;
    }
        
    snd_pcm_hw_params_free(hwparams);
    return true;

error:
    LOGERR("alsa_init: " << cmd << " error:" << snd_strerror(err) << endl);
    snd_pcm_hw_params_free(hwparams);
    return false;
}

// Current in-driver delay in samples
static int alsadelay()
{
    snd_pcm_sframes_t delay;
    if (snd_pcm_delay(pcm, &delay) >= 0) {
        return delay;
    } else {
        return 0;
    }
}

class Filter {
public:
#define FNS 128
    Filter() : old(0.0), sum(0.0), idx(0) {
        for (int i = 0; i < FNS; i++) {
            buf[i] = 1.0;
            sum += buf[i];
        }
    }
    double operator()(double ns) {
        buf[idx++] = ns;
        sum += ns;
        if (idx == FNS)
            idx = 0;
        sum -= buf[idx];
        return sum/FNS;
    }
    double old;
    double buf[FNS];
    double sum;
    int idx;
};

// Convert config parameter to libsamplerate converter type
static int src_cvt_type(ConfSimple *config)
{
    int tp = SRC_SINC_FASTEST;
    if (!config)
        return tp;
    string value;
    if (!config->get("sccvttype", value))
        return tp;
    LOGDEB("src_cvt_type. conf string [" << value << "]\n");
    if (!value.compare("SRC_SINC_BEST_QUALITY")) {
        tp = SRC_SINC_BEST_QUALITY;
    } else if (!value.compare("SRC_SINC_MEDIUM_QUALITY")) {
        tp = SRC_SINC_MEDIUM_QUALITY;
    } else if (!value.compare("SRC_SINC_FASTEST")) {
        tp = SRC_SINC_FASTEST;
    } else if (!value.compare("SRC_ZERO_ORDER_HOLD")) {
        tp = SRC_ZERO_ORDER_HOLD;
    } else if (!value.compare("SRC_LINEAR")) {
        tp = SRC_LINEAR;
    } else {
        // Allow numeric values for transparent expansion to
        // hypothetic libsamplerate updates (allowing this is explicit
        // in the libsamplerate doc).
        long int lval;
        char *cp;
        lval = strtol(value.c_str(), &cp, 10);
        if (cp != value.c_str()) {
            tp = int(lval);
        } else {
            LOGERR("Invalid converter type [" << value << 
                   "] using SRCC_SINC_FASTEST" << endl);
        }
    }
    return tp;

}
static void *audioEater(void *cls)
{
    AudioEater::Context *ctxt = (AudioEater::Context*)cls;

    int cvt_type = src_cvt_type(ctxt->config);
    LOGDEB("audioEater: alsadirect. Will use converter type " << 
           cvt_type << endl);

    string alsadevice("default");
    ctxt->config->get("scalsadevice", alsadevice);    

    WorkQueue<AudioMessage*> *queue = ctxt->queue;

    delete ctxt;
    ctxt = 0;

    qinit = false;

    double samplerate_ratio = 1.0;
    Filter filter;

    int src_error = 0;
    SRC_STATE *src_state = 0;
    SRC_DATA src_data;
    memset(&src_data, 0, sizeof(src_data));
    // Current size of the samplerate input buffer. We always alloc
    // twice the size for output (allocated on first use).
    size_t src_input_bytes = 0;
    
    alsaqueue.start(1, alsawriter, 0);

    // Integral term. We do not use it at the moment
    // double it = 0;

    // Number of frames per buffer. This is mostly constant for a
    // given stream (depends on fe and buffer time, Windows Songcast
    // buffers are 10mS, so 441 frames at cd q). Recomputed on first
    // buf, the init is to avoid warnings
    int bufframes = 441;

    while (true) {
        AudioMessage *tsk = 0;
        size_t qsz;
        if (!queue->take(&tsk, &qsz)) {
            LOGDEB("audioEater: alsadirect: queue take failed\n");
            alsaqueue.setTerminateAndWait();
            queue->workerExit();
            return (void*)1;
        }

        if (tsk->m_bytes == 0 || tsk->m_chans == 0 || tsk->m_bits == 0) {
            LOGDEB("Zero buf\n");
            continue;
        }

        if (src_state == 0) {
            if (!alsa_init(alsadevice, tsk)) {
                alsaqueue.setTerminateAndWait();
                queue->workerExit();
                return (void *)1;
            }
            // BEST_QUALITY yields approx 25% cpu on a core i7
            // 4770T. Obviously too much, actually might not be
            // sustainable (it's almost 100% of 1 cpu)
            // MEDIUM_QUALITY is around 10%
            // FASTEST is 4-5%. Given that this measured for the full
            // process, probably a couple % for the conversion in fact.
            // Rpi: FASTEST is 30% CPU on a Pi2 with USB
            // audio. Curiously it's 25-30% on a Pi1 with i2s audio.
            src_state = src_new(cvt_type, tsk->m_chans, &src_error);

            bufframes = tsk->frames();
        }
        
        // Computing the samplerate conversion factor. We want to keep
        // the queue at its target size to control the delay. The
        // present hack sort of works but could probably benefit from
        // a more scientific approach

        // Qsize in frames. This is the variable to control
        double qs;

        if (qinit) {
            qs = alsaqueue.qsize() * bufframes + alsadelay();
            // Error term
            double qstargframes = qstarg * bufframes;
            double et =  ((qstargframes - qs) / qstargframes);

            // Integral. Not used, made it worse each time I tried.
            // This is probably because our command is actually the
            // derivative of the error? I should try a derivative term
            // instead?
            // it += et;

            // Error correction coef
            double ce = 0.1;

            // Integral coef
            //double ci = 0.0001;

            // Compute command
            double adj = ce * et /* + ci * it*/;

            // Also tried a quadratic correction, worse.
            // double adj = et * ((et < 0) ? -et : et);

            // Computed ratio
            samplerate_ratio =  1.0 + adj;

            // Limit extension
            if (samplerate_ratio < 0.9) 
                samplerate_ratio = 0.9;
            if (samplerate_ratio > 1.1)
                samplerate_ratio = 1.1;

        } else {
            // Starting up, wait for more info
            qs = alsaqueue.qsize();
            samplerate_ratio = 1.0;
            // it = 0;
        }

        // Average the rate value to eliminate fast oscillations
        samplerate_ratio = filter(samplerate_ratio);

        unsigned int tot_samples = tsk->samples();
        src_data.input_frames = tsk->frames();
        size_t needed_bytes = tot_samples * sizeof(float);
        if (src_input_bytes < needed_bytes) {
            src_data.data_in = (float *)realloc(src_data.data_in, needed_bytes);
            src_data.data_out = (float *)realloc(src_data.data_out,
                                                 2 * needed_bytes);
            src_data.output_frames = 2 * tot_samples / tsk->m_chans;
            src_input_bytes = needed_bytes;
        }

        src_data.src_ratio = samplerate_ratio;
        src_data.end_of_input = 0;
        
        // Data always comes in host order, because this is what we
        // request from upstream. 24 and 32 bits are untested.
        switch (tsk->m_bits) {
        case 16: 
        {
            const short *sp = (const short *)tsk->m_buf;
            for (unsigned int i = 0; i < tot_samples; i++) {
                src_data.data_in[i] = *sp++;
            }
        }
        break;
        case 24: 
        {
            const unsigned char *icp = (const unsigned char *)tsk->m_buf;
            int o;
            unsigned char *ocp = (unsigned char *)&o;
            for (unsigned int i = 0; i < tot_samples; i++) {
                ocp[0] = *icp++;
                ocp[1] = *icp++;
                ocp[2] = *icp++;
                ocp[3] = (ocp[2] & 0x80) ? 0xff : 0;
                src_data.data_in[i] = o;
            }
        }
        break;
        case 32: 
        {
            const int *ip = (const int *)tsk->m_buf;
            for (unsigned int i = 0; i < tot_samples; i++) {
                src_data.data_in[i] = *ip++;
            }
        }
        break;
        default:
            LOGERR("audioEater:alsa: bad m_bits: " << tsk->m_bits << endl);
            alsaqueue.setTerminateAndWait();
            queue->workerExit();
            return (void *)1;
        }

        int ret = src_process(src_state, &src_data);
        if (ret) {
            LOGERR("src_process: " << src_strerror(ret) << endl);
            continue;
        }

        {
            static int cnt;
            if (cnt++ == 103) {
                LOGDEB("audioEater:alsa: " 
                       " qstarg " << qstarg <<
                       " iqsz " << alsaqueue.qsize() <<
                       " qsize " << int(qs/bufframes) << 
                       " ratio " << samplerate_ratio <<
                       " in " << src_data.input_frames << 
                       " consumed " << src_data.input_frames_used << 
                       " out " << src_data.output_frames_gen << endl);
                cnt = 0;
            }
        }

        // New number of samples after conversion. We are going to
        // copy them back to the audio buffer, and may need to
        // reallocate it.
        tot_samples =  src_data.output_frames_gen * tsk->m_chans;
        needed_bytes = tot_samples * (tsk->m_bits / 8);
        if (tsk->m_allocbytes < needed_bytes) {
            tsk->m_allocbytes = needed_bytes;
            tsk->m_buf = (char *)realloc(tsk->m_buf, tsk->m_allocbytes);
            if (!tsk->m_buf) {
                LOGERR("audioEater:alsa: out of memory\n");
                alsaqueue.setTerminateAndWait();
                queue->workerExit();
                return (void *)1;
            }
        }

        // Convert floats buffer into output which is always 16LE for
        // now. We should probably dither the lsb ?
	// The libsamplerate output values can overshoot the input range 
	// (see http://www.mega-nerd.com/SRC/faq.html#Q001), so we take care 
	// to clip the values.
        {
            short *sp = (short *)tsk->m_buf;
            switch (tsk->m_bits) {
            case 16:
            {
                for (unsigned int i = 0; i < tot_samples; i++) {
                    int v = src_data.data_out[i];
                    if (v > 32767) {
                        v = 32767;
                    } else if (v < -32768) {
                        v = -32768;
                    }
                    *sp++ = BSWAP16(short(v));
                }
            }
            break;
            case 24:
            {
                for (unsigned int i = 0; i < tot_samples; i++) {
                    int v = src_data.data_out[i];
                    if (v > (1 << 23) - 1) {
                        v = (1 << 23) - 1;
                    } else if (v < -(1 << 23)) {
                        v = -(1 << 23);
                    }
                    *sp++ = BSWAP16(short(v >> 8));
                }
            }
            break;
            case 32:
            {
                for (unsigned int i = 0; i < tot_samples; i++) {
                    float& f = src_data.data_out[i];
                    int v = f;
                    if (f > 0 && v < 0) {
                        v = unsigned(1 << 31) - 1;
                    } else if (f < 0 && v > 0) {
                        v = -unsigned(1 << 31);
                    }
                    *sp++ = BSWAP16(short(v >> 16));
                }
            }
            break;
            default:
                LOGERR("audioEater:alsa: bad m_bits: " << tsk->m_bits << endl);
                alsaqueue.setTerminateAndWait();
                queue->workerExit();
                return (void *)1;
            }
            tsk->m_bytes = (char *)sp - tsk->m_buf;
        }
        tsk->m_bits = 16;

        if (!alsaqueue.put(tsk)) {
            LOGERR("alsaEater: queue put failed\n");
            queue->workerExit();
            return (void *)1;
        }
    }
}

AudioEater alsaAudioEater(AudioEater::BO_HOST, &audioEater);
