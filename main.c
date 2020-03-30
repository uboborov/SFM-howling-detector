#include <stdio.h>
#include <stdint.h>
#include <unistd.h>
#include <linux/serial.h>
#include <termios.h>
#include <stdlib.h>
#include <pthread.h>
#include <errno.h>
#include <string.h>
#include <sys/stat.h>
#include <signal.h>
#include <alsa/asoundlib.h>
#include <alsa/mixer.h>

#include <NE10/NE10.h>
#include <siglib.h>
#include <nhl.h>
#include <time.h>

#define DEBUG

#ifdef DEBUG
# define DBG_PRINT printf
#else
# define DBG_PRINT(...)
#endif



#define FFT_SIZE 1024
#define SIGNAL_MAGNITUDE    1.0
#define FREQUENCY           100.0
#define SAMPLE_RATE         8000.0
#define SIGNAL_FREQUENCY    FREQUENCY/SAMPLE_RATE   /* Normalised to 1.0 Hz */

#define BUFSIZE (320)
#define PERIOD_SIZE             160
#define CHANNELS                1
#define BITS                    16

static SFLOAT *pFDPResults;
static SFLOAT ZerodBmLevel;
static SFLOAT *pWindowCoeffs;
static SFLOAT *pFreq;
static SFLOAT *pFFTCoeffs;
static SFLOAT *pImagData;
static SFLOAT *pRealData;
static SFIX Log2Size;
static short  *frame_buf;
static SFLOAT *pTmp;
static ComplexRect_s *fshift;
static ComplexRect_s *ik;
static double *pdouble;

static ne10_fft_r2c_cfg_float32_t icfg;
ne10_float32_t *isrc = NULL;
ne10_fft_cpx_float32_t *idst = NULL;
ne10_fft_cpx_float32_t *ikn = NULL;

static unsigned int rate = SAMPLE_RATE;
snd_pcm_t *playback_handle, *capture_handle;
FILE *pInFile, *pOutFile;
static WAV_FILE_INFO wav_in_info, wav_in1_info, wav_out_info;
pthread_t pb_thread;

struct _pbuf {
    short *buf;
    int dlen;
};

static struct _pbuf inbuf, outbuf;


static int open_stream(snd_pcm_t **handle, const char *name, int dir, int *pframes) {
    snd_pcm_hw_params_t *hw_params;
    snd_pcm_sw_params_t *sw_params;
    const char *dirname = (dir == SND_PCM_STREAM_PLAYBACK) ? "PLAYBACK" : "CAPTURE";
    int err;
    snd_pcm_uframes_t frames;
    int fragments = 2, frame_size = 2;
    int ctr = 0, _dir;

    while (snd_pcm_open(handle, name, dir, 0) < 0) {
        usleep(10000);
        if (ctr++ > 500) {
            DBG_PRINT("Failed to open device %s for %s\n", name, dirname);
            return -1;
        }

    }
       
    if ((err = snd_pcm_hw_params_malloc(&hw_params)) < 0) {
        DBG_PRINT("%s (%s): cannot allocate hardware parameter structure(%s)\n",
            name, dirname, snd_strerror(err));
        return err;
    }
             
    if ((err = snd_pcm_hw_params_any(*handle, hw_params)) < 0) {
        DBG_PRINT("%s (%s): cannot initialize hardware parameter structure(%s)\n",
            name, dirname, snd_strerror(err));
        return err;
    }

    if ((err = snd_pcm_hw_params_set_access(*handle, hw_params, SND_PCM_ACCESS_RW_INTERLEAVED)) < 0) {
        DBG_PRINT("%s (%s): cannot set access type(%s)\n",
            name, dirname, snd_strerror(err));
        return err;
    }

    if ((err = snd_pcm_hw_params_set_format(*handle, hw_params, SND_PCM_FORMAT_S16_LE)) < 0) {
        DBG_PRINT("%s (%s): cannot set sample format(%s)\n",
            name, dirname, snd_strerror(err));
        return err;
    }

    if ((err = snd_pcm_hw_params_set_rate_near(*handle, hw_params, &rate, NULL)) < 0) {
        DBG_PRINT("%s (%s): cannot set sample rate(%s)\n",
            name, dirname, snd_strerror(err));
        return err;
    }

    if ((err = snd_pcm_hw_params_set_channels(*handle, hw_params, CHANNELS)) < 0) {
        DBG_PRINT("%s (%s): cannot set channel count(%s)\n",
            name, dirname, snd_strerror(err));
        return err;
    }

    //*************** This makes latency low *****************************
    if ((err = snd_pcm_hw_params_set_periods_near(*handle, hw_params, &fragments, 0)) < 0) {
        DBG_PRINT("Error setting # fragments to %d: %s\n", fragments,
                snd_strerror(err));
        return err;
    }

    frame_size = CHANNELS * (BITS / 8);
    frames = PERIOD_SIZE / frame_size * fragments;
    if ((err = snd_pcm_hw_params_set_buffer_size_near(*handle, hw_params, &frames)) < 0) {
        DBG_PRINT("Error setting buffer_size %d frames: %s\n", frames,
                snd_strerror(err));
        return err;
    }

    if (pframes) {
        //*pframes = frames;
        snd_pcm_hw_params_get_period_size(hw_params, &frames, &_dir);
        *pframes = frames;
    }
    //************************************************************

    if ((err = snd_pcm_hw_params(*handle, hw_params)) < 0) {
        DBG_PRINT("%s (%s): cannot set parameters(%s)\n",
            name, dirname, snd_strerror(err));
        return err;
    }

    snd_pcm_hw_params_free(hw_params);

    if ((err = snd_pcm_sw_params_malloc(&sw_params)) < 0) {
        DBG_PRINT("%s (%s): cannot allocate software parameters structure(%s)\n",
            name, dirname, snd_strerror(err));
        return err;
    }
    if ((err = snd_pcm_sw_params_current(*handle, sw_params)) < 0) {
        DBG_PRINT("%s (%s): cannot initialize software parameters structure(%s)\n",
            name, dirname, snd_strerror(err));
        return err;
    }
    if ((err = snd_pcm_sw_params_set_start_threshold(*handle, sw_params, 0)) < 0) {
        DBG_PRINT("%s (%s): cannot set start mode(%s)\n",
            name, dirname, snd_strerror(err));
        return err;
    }
    // makes driver interrupted on BUFSIZE frames ready
    if ((err = snd_pcm_sw_params_set_avail_min(*handle, sw_params, frames)) < 0) {
        DBG_PRINT("%s (%s): cannot set minimum available count(%s)\n",
            name, dirname, snd_strerror(err));
        return err;
    }
    
    if ((err = snd_pcm_sw_params(*handle, sw_params)) < 0) {
        DBG_PRINT("%s (%s): cannot set software parameters(%s)\n",
            name, dirname, snd_strerror(err));
        return err;
    }

    return 0;
}

/*
 *
 */
void open_wav_file(char *fname) {
    pInFile = fopen(fname,"rb");
    if (!pInFile) {
        printf("File open filed: %s\n", fname);
        exit(-1);
    }

    wav_in_info = wav_read_header(pInFile);
    wav_display_info(wav_in_info);
}

/*
 *
 */
void init_ne10_dsp() {
    ne10_init();
    icfg = ne10_fft_alloc_r2c_float32(FFT_SIZE);
    isrc = malloc(FFT_SIZE * sizeof(ne10_float32_t)); 
    idst = malloc(FFT_SIZE * sizeof(ne10_fft_cpx_float32_t));
    //ikn = malloc(FFT_SIZE * sizeof(ne10_fft_cpx_float32_t));
} 

/*
 *
 */
void init_siglib_dsp() {
    pFDPResults = SUF_VectorArrayAllocate(FFT_SIZE);
    ZerodBmLevel = SDS_BitsToPeakValue (16.0, SIGLIB_SIGNED_DATA);
    pWindowCoeffs = SUF_VectorArrayAllocate(FFT_SIZE);
    SIF_Window(pWindowCoeffs, SIGLIB_HANNING, SIGLIB_ZERO, FFT_SIZE);
    pImagData = SUF_VectorArrayAllocate(FFT_SIZE);
    pRealData = SUF_VectorArrayAllocate(FFT_SIZE);
    //pFreq = SUF_VectorArrayAllocate(FFT_SIZE);
    //pFFTCoeffs = SUF_FftCoefficientAllocate(FFT_SIZE);
    //SIF_Fft(pFFTCoeffs, SIGLIB_NULL_SFIX_PTR, FFT_SIZE);
    //Log2Size = (SFIX)(SDS_Log2(FFT_SIZE) + 0.1L);
    //frame_buf = (short *)malloc(FFT_SIZE * sizeof(short));
    //pTmp = SUF_VectorArrayAllocate(FFT_SIZE);
    //fshift = SUF_ComplexRectArrayAllocate(FFT_SIZE);
    //ik = SUF_ComplexRectArrayAllocate(FFT_SIZE);

    //pdouble = (double *)malloc(sizeof(double)*FFT_SIZE);
}

/*
 *
 */
void init_buffers() {
    inbuf.buf = (short *)malloc(sizeof(short) * FFT_SIZE * 2);
    inbuf.dlen = 0;

    outbuf.buf = (short *)malloc(sizeof(short) * FFT_SIZE * 2);
    outbuf.dlen = 0;
}

/*
 *
 */
int read_samples(short *in, WAV_FILE_INFO *info, int len) {
    return wav_read_data16(in, pInFile, *info, len);
}

/*
 *
 */
float sfm2(SFLOAT *input, int nSamples, int offset) {
  double geommean = 1;
  double mean = 0.0;
  double absVal;
  float sfmVal;
  int i;
  int shift = offset; //

  for (i = shift;i < nSamples;i++) {
    absVal = (double)input[i];
    geommean *= (absVal > 1e-6)? absVal:1;
    mean += absVal;
  }
  geommean = (double) pow(geommean,(1.f/((double)nSamples)));
  mean /= (double)nSamples;
  sfmVal = (float) (geommean/mean);

  return sfmVal;
}

/*
 *
 */
double sfm_db(SFLOAT *pwr_spec, int num) {
    double  alpha;
    double  a_mean;     /* arithmetic mean */
    double  g_mean;     /* geometric mean */
    int     i;
    int     shift = 1;

    a_mean = 0;
    g_mean = 0;

    for (i = shift; i < num; i++) {
        a_mean += (double)pwr_spec[i];
        g_mean += log((double)pwr_spec[i]);
    }

    a_mean = a_mean / (double) num;
    g_mean = exp(g_mean / (double) num);

    alpha = 10.0 * log10(g_mean / a_mean) / (-60.0);

    if (alpha > 1.0)
        alpha = 1.0;

    return alpha;
}

/*
 *
 */
int init_playback() {
    int err;
    int frames = PERIOD_SIZE;

    if ((err = open_stream(&playback_handle, "default", SND_PCM_STREAM_PLAYBACK, &frames)) < 0) {
        DBG_PRINT("cannot open stream (%s) => exit\n", "PB");
        return -1;
    }

    if ((err = snd_pcm_prepare(playback_handle)) < 0) {
        DBG_PRINT("cannot prepare audio interface for use(%s)\n", snd_strerror(err));
        return -1;
    }

    return frames;
}

/*
 *
 */
int write_samples_pb(short *buf, int datasz, int frames, WAV_FILE_INFO *info) {
    int err, avail;

    int b, bptr = 0;
    int nblocks = datasz / frames;
    int nframes = datasz % frames;

    for (b = 0;b < nblocks;b++) {
        bptr = b*frames;//*info->BytesPerSample;

        if ((err = snd_pcm_wait(playback_handle, 1000)) < 0) {
            DBG_PRINT("poll failed(%s)\n", strerror(errno));
            snd_pcm_prepare(playback_handle);
        }    

        avail = snd_pcm_avail_update(playback_handle);
        if (avail > 0) {
            if (avail > frames)
                avail = frames;

            snd_pcm_writei(playback_handle, buf+bptr, avail);
        }        
    }

    if (datasz == FFT_SIZE && nframes > 0) {
        fseek(pInFile, -(nframes*info->BytesPerSample), SEEK_CUR);
    }

    return 0; 
}

/*
 *  If flatness less than SFM_THR => howling or harmonic signal detected
 */

#define SFM_THR         (float)0.15
#define NBANDS          32 // subands per 4000 Hz
#define NBSIZE          (FFT_SIZE/2/NBANDS)
#define SOFFSET(band)   ((band)*NBSIZE)

float howling_hyst[NBANDS] = {0};
/*
 *
 */
int process_samples(short *in, short *out, int len) {
    int i, h_flag = 0;
    ne10_vec2f_t tmp_vec;
    SFLOAT sum;
    float fTmp = 0.0, flatness;

    // 1. normalize input data
    for (i = 0;i < len;i++) {    
        pRealData[i] = (SFLOAT)((SFLOAT)in[i] / (SFLOAT)ZerodBmLevel);
    }

    // 2. Hanning window
    SDA_Window(pRealData, pRealData, pWindowCoeffs, len);

    for (i = 0; i < len; i++) {
        isrc[i] = (ne10_float32_t)pRealData[i];
    }

    // 2. FFT real-to-complex
    ne10_fft_r2c_1d_float32_neon(idst, isrc, icfg);

    // 3. scale FFT result
    tmp_vec.x = (ne10_float32_t)FFT_SIZE;
    tmp_vec.y = (ne10_float32_t)FFT_SIZE;
    ne10_divc_vec2f_neon((ne10_vec2f_t *)idst, (ne10_vec2f_t *)idst, &tmp_vec, FFT_SIZE);

    // 4. calc magnitude
    for (i = 0; i < len; i++) {
        pRealData[i] = (SFLOAT)idst[i].r;
        pImagData[i] = (SFLOAT)idst[i].i;
    }

    SDA_Magnitude(pRealData, pImagData, pFDPResults, len);

    // 5. calc spectral flatness
    h_flag = 0;

    // calculate flatness per band
    for (i = 0;i < NBANDS;i++) {
        flatness = sfm2(&pFDPResults[SOFFSET(i)], NBSIZE, (i == 0)?1:0);
        if (flatness < SFM_THR) {
            printf("band %d: %f\n", i, flatness);
            howling_hyst[i] = (howling_hyst[i] > flatness)?howling_hyst[i]:flatness;
            h_flag++;
        }
    }

    // supress frames with howling
    if (h_flag == 0)
        memcpy(out, in, len * sizeof(short));
    else
        memset(out, 0, len * sizeof(short));

}

/*
 *
 */
void print_band_howling() {
    int i;
    int bl, bu, bw;
    float fbin;

    for (i = 0;i < NBANDS;i++) {
        bw = FFT_SIZE/2/NBANDS;
        fbin = ((float)SAMPLE_RATE/(float)2)/((float)FFT_SIZE/(float)2);
        bl = bw*fbin*i;
        bu = bw*fbin*(i+1);
        printf("%4d-%4d -> %f %f\n", bl, bu, howling_hyst[i], fbin);
    }
}


int main(int argc, char *argv[]) {
    int frames, rdcnt;
    int i;
    char *pf = "1000Hz8000.wav";
    
    if (argc > 1) {
        // file to open
        pf = argv[1];
    }
    
    // open sound file
    open_wav_file(pf);
    
    // init NE10
    init_ne10_dsp();

    // init SIGIB
    init_siglib_dsp();

    // init buffers
    init_buffers();

    // init pb
    if ((frames = init_playback()) < 0) {
        printf("PB init failed\n");
        return 0;
    }

    // read file with speech/howling, detect and suppress howling
    while((rdcnt = read_samples(inbuf.buf, &wav_in_info, FFT_SIZE)) > 0) {
        process_samples(inbuf.buf, outbuf.buf, FFT_SIZE);
        write_samples_pb(outbuf.buf, rdcnt, frames, &wav_in_info);
    }

    print_band_howling();

    printf("Done!\n");

    return 0;
}
