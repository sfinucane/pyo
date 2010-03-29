/*************************************************************************
 * Copyright 2010 Olivier Belanger                                        *                  
 *                                                                        * 
 * This file is part of pyo, a python module to help digital signal       *
 * processing script creation.                                            *  
 *                                                                        * 
 * pyo is free software: you can redistribute it and/or modify            *
 * it under the terms of the GNU General Public License as published by   *
 * the Free Software Foundation, either version 3 of the License, or      *
 * (at your option) any later version.                                    * 
 *                                                                        *
 * pyo is distributed in the hope that it will be useful,                 *
 * but WITHOUT ANY WARRANTY; without even the implied warranty of         *    
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the          *
 * GNU General Public License for more details.                           *
 *                                                                        *
 * You should have received a copy of the GNU General Public License      *
 * along with pyo.  If not, see <http://www.gnu.org/licenses/>.           *
 *************************************************************************/

#include <Python.h>
#include "structmember.h"
#include <math.h>
#include "pyomodule.h"
#include "streammodule.h"
#include "servermodule.h"
#include "dummymodule.h"

#define DEFAULT_SRATE   44100.0
#define NUM_COMB         8
#define NUM_ALLPASS      4

static const float comb_delays[NUM_COMB] = {
1116.0 / DEFAULT_SRATE, 
1188.0 / DEFAULT_SRATE, 
1277.0 / DEFAULT_SRATE, 
1356.0 / DEFAULT_SRATE,
1422.0 / DEFAULT_SRATE, 
1491.0 / DEFAULT_SRATE, 
1557.0 / DEFAULT_SRATE, 
1617.0 / DEFAULT_SRATE
};

static const float allpass_delays[NUM_ALLPASS] = {
556.0 / DEFAULT_SRATE, 
441.0 / DEFAULT_SRATE, 
341.0 / DEFAULT_SRATE, 
225.0 / DEFAULT_SRATE
};

static const float fixedGain   = 0.015;
static const float scaleDamp   = 0.5;
static const float scaleRoom   = 0.28;
static const float offsetRoom  = 0.7;
static const float allPassFeedBack = 0.5;

typedef struct {
    pyo_audio_HEAD
    PyObject *input;
    Stream *input_stream;
    PyObject *size;
    Stream *size_stream;
    PyObject *damp;
    Stream *damp_stream;
    PyObject *mix;
    Stream *mix_stream;
    int comb_nSamples[NUM_COMB];
    int comb_bufPos[NUM_COMB];
    float comb_filterState[NUM_COMB];
    float *comb_buf[NUM_COMB];
    int allpass_nSamples[NUM_ALLPASS];
    int allpass_bufPos[NUM_ALLPASS];
    float *allpass_buf[NUM_ALLPASS];
    int modebuffer[5];
    float srFactor;
} Freeverb;

static float
_clip(float x)
{
    if (x < 0)
        return 0;
    else if (x > 1)
        return 1;
    else
        return x;
}

static int 
Freeverb_calc_nsamples(Freeverb *self, float delTime)
{
    return (int)(delTime * self->sr + 0.5);
}

static void
Freeverb_transform_iii(Freeverb *self) {
    float x, feedback, damp1, damp2, mix1, mix2;
    int i, j;

    float *in = Stream_getData((Stream *)self->input_stream);
    float siz = _clip(PyFloat_AS_DOUBLE(self->size));
    float dam = _clip(PyFloat_AS_DOUBLE(self->damp));
    float mix = _clip(PyFloat_AS_DOUBLE(self->mix));

    feedback = siz * scaleRoom + offsetRoom;
    damp1 = dam * scaleDamp;
    damp2 = 1.0 - damp1;

    mix1 = sqrtf(mix);
    mix2 = sqrtf(1.0 - mix);
    
    float tmp[self->bufsize];
    memset(&tmp, 0, sizeof(tmp));
    
    for (j=0; j<self->bufsize; j++) {
        for (i=0; i<NUM_COMB; i++) {
            x = self->comb_buf[i][self->comb_bufPos[i]];
            tmp[j] += x;
            self->comb_filterState[i] = (self->comb_filterState[i] * damp1) + (x * damp2);
            x = self->comb_filterState[i] * feedback + in[j];
            self->comb_buf[i][self->comb_bufPos[i]] = x;
            self->comb_bufPos[i]++;
            if (self->comb_bufPos[i] >= self->comb_nSamples[i])
                self->comb_bufPos[i] = 0;
        }
    }
    
    for (i=0; i<NUM_ALLPASS; i++) {
        for (j=0; j<self->bufsize; j++) {
            x = self->allpass_buf[i][self->allpass_bufPos[i]] - tmp[j];
            self->allpass_buf[i][self->allpass_bufPos[i]] *= allPassFeedBack;
            self->allpass_buf[i][self->allpass_bufPos[i]] += tmp[j];
            self->allpass_bufPos[i]++;
            if (self->allpass_bufPos[i] >= self->allpass_nSamples[i])
                self->allpass_bufPos[i] = 0;
            tmp[j] = x;
        }    
    }
    
    for (i=0; i<self->bufsize; i++) {
        self->data[i] = (tmp[i] * fixedGain * mix1) + (in[i] * mix2);
    }    
}

static void
Freeverb_transform_aii(Freeverb *self) {
    float x, feedback, damp1, damp2, mix1, mix2;
    int i, j;
    
    float *in = Stream_getData((Stream *)self->input_stream);
    float *siz = Stream_getData((Stream *)self->size_stream);
    float dam = _clip(PyFloat_AS_DOUBLE(self->damp));
    float mix = _clip(PyFloat_AS_DOUBLE(self->mix));
    
    damp1 = dam * scaleDamp;
    damp2 = 1.0 - damp1;
    
    mix1 = sqrtf(mix);
    mix2 = sqrtf(1.0 - mix);
    
    float tmp[self->bufsize];
    memset(&tmp, 0, sizeof(tmp));
    
    for (j=0; j<self->bufsize; j++) {
        feedback = _clip(siz[j]) * scaleRoom + offsetRoom;
        for (i=0; i<NUM_COMB; i++) {
            x = self->comb_buf[i][self->comb_bufPos[i]];
            tmp[j] += x;
            self->comb_filterState[i] = (self->comb_filterState[i] * damp1) + (x * damp2);
            x = self->comb_filterState[i] * feedback + in[j];
            self->comb_buf[i][self->comb_bufPos[i]] = x;
            self->comb_bufPos[i]++;
            if (self->comb_bufPos[i] >= self->comb_nSamples[i])
                self->comb_bufPos[i] = 0;
        }
    }
    
    for (i=0; i<NUM_ALLPASS; i++) {
        for (j=0; j<self->bufsize; j++) {
            x = self->allpass_buf[i][self->allpass_bufPos[i]] - tmp[j];
            self->allpass_buf[i][self->allpass_bufPos[i]] *= allPassFeedBack;
            self->allpass_buf[i][self->allpass_bufPos[i]] += tmp[j];
            self->allpass_bufPos[i]++;
            if (self->allpass_bufPos[i] >= self->allpass_nSamples[i])
                self->allpass_bufPos[i] = 0;
            tmp[j] = x;
        }    
    }
    
    for (i=0; i<self->bufsize; i++) {
        self->data[i] = (tmp[i] * fixedGain * mix1) + (in[i] * mix2);
    }    
}

static void
Freeverb_transform_iai(Freeverb *self) {
    float x, feedback, damp1, damp2, mix1, mix2;
    int i, j;
    
    float *in = Stream_getData((Stream *)self->input_stream);
    float siz = _clip(PyFloat_AS_DOUBLE(self->size));
    float *dam = Stream_getData((Stream *)self->damp_stream);
    float mix = _clip(PyFloat_AS_DOUBLE(self->mix));
    
    feedback = siz * scaleRoom + offsetRoom;
    
    mix1 = sqrtf(mix);
    mix2 = sqrtf(1.0 - mix);
    
    float tmp[self->bufsize];
    memset(&tmp, 0, sizeof(tmp));
    
    for (j=0; j<self->bufsize; j++) {
        damp1 = _clip(dam[j]) * scaleDamp;
        damp2 = 1.0 - damp1;
        for (i=0; i<NUM_COMB; i++) {
            x = self->comb_buf[i][self->comb_bufPos[i]];
            tmp[j] += x;
            self->comb_filterState[i] = (self->comb_filterState[i] * damp1) + (x * damp2);
            x = self->comb_filterState[i] * feedback + in[j];
            self->comb_buf[i][self->comb_bufPos[i]] = x;
            self->comb_bufPos[i]++;
            if (self->comb_bufPos[i] >= self->comb_nSamples[i])
                self->comb_bufPos[i] = 0;
        }
    }
    
    for (i=0; i<NUM_ALLPASS; i++) {
        for (j=0; j<self->bufsize; j++) {
            x = self->allpass_buf[i][self->allpass_bufPos[i]] - tmp[j];
            self->allpass_buf[i][self->allpass_bufPos[i]] *= allPassFeedBack;
            self->allpass_buf[i][self->allpass_bufPos[i]] += tmp[j];
            self->allpass_bufPos[i]++;
            if (self->allpass_bufPos[i] >= self->allpass_nSamples[i])
                self->allpass_bufPos[i] = 0;
            tmp[j] = x;
        }    
    }
    
    for (i=0; i<self->bufsize; i++) {
        self->data[i] = (tmp[i] * fixedGain * mix1) + (in[i] * mix2);
    }    
}

static void
Freeverb_transform_aai(Freeverb *self) {
    float x, feedback, damp1, damp2, mix1, mix2;
    int i, j;
    
    float *in = Stream_getData((Stream *)self->input_stream);
    float *siz = Stream_getData((Stream *)self->size_stream);
    float *dam = Stream_getData((Stream *)self->damp_stream);
    float mix = _clip(PyFloat_AS_DOUBLE(self->mix));

    mix1 = sqrtf(mix);
    mix2 = sqrtf(1.0 - mix);

    float tmp[self->bufsize];
    memset(&tmp, 0, sizeof(tmp));
    
    for (j=0; j<self->bufsize; j++) {
        feedback = _clip(siz[j]) * scaleRoom + offsetRoom;
        damp1 = _clip(dam[j]) * scaleDamp;
        damp2 = 1.0 - damp1;
        for (i=0; i<NUM_COMB; i++) {
            x = self->comb_buf[i][self->comb_bufPos[i]];
            tmp[j] += x;
            self->comb_filterState[i] = (self->comb_filterState[i] * damp1) + (x * damp2);
            x = self->comb_filterState[i] * feedback + in[j];
            self->comb_buf[i][self->comb_bufPos[i]] = x;
            self->comb_bufPos[i]++;
            if (self->comb_bufPos[i] >= self->comb_nSamples[i])
                self->comb_bufPos[i] = 0;
        }
    }
    
    for (i=0; i<NUM_ALLPASS; i++) {
        for (j=0; j<self->bufsize; j++) {
            x = self->allpass_buf[i][self->allpass_bufPos[i]] - tmp[j];
            self->allpass_buf[i][self->allpass_bufPos[i]] *= allPassFeedBack;
            self->allpass_buf[i][self->allpass_bufPos[i]] += tmp[j];
            self->allpass_bufPos[i]++;
            if (self->allpass_bufPos[i] >= self->allpass_nSamples[i])
                self->allpass_bufPos[i] = 0;
            tmp[j] = x;
        }    
    }
    
    for (i=0; i<self->bufsize; i++) {
        self->data[i] = (tmp[i] * fixedGain * mix1) + (in[i] * mix2);
    }    
}

static void
Freeverb_transform_iia(Freeverb *self) {
    float x, feedback, damp1, damp2, mix1, mix2, mixtmp;
    int i, j;
    
    float *in = Stream_getData((Stream *)self->input_stream);
    float siz = _clip(PyFloat_AS_DOUBLE(self->size));
    float dam = _clip(PyFloat_AS_DOUBLE(self->damp));
    float *mix = Stream_getData((Stream *)self->mix_stream);
    
    feedback = siz * scaleRoom + offsetRoom;
    damp1 = dam * scaleDamp;
    damp2 = 1.0 - damp1;

    float tmp[self->bufsize];
    memset(&tmp, 0, sizeof(tmp));
    
    for (j=0; j<self->bufsize; j++) {
        for (i=0; i<NUM_COMB; i++) {
            x = self->comb_buf[i][self->comb_bufPos[i]];
            tmp[j] += x;
            self->comb_filterState[i] = (self->comb_filterState[i] * damp1) + (x * damp2);
            x = self->comb_filterState[i] * feedback + in[j];
            self->comb_buf[i][self->comb_bufPos[i]] = x;
            self->comb_bufPos[i]++;
            if (self->comb_bufPos[i] >= self->comb_nSamples[i])
                self->comb_bufPos[i] = 0;
        }
    }
    
    for (i=0; i<NUM_ALLPASS; i++) {
        for (j=0; j<self->bufsize; j++) {
            x = self->allpass_buf[i][self->allpass_bufPos[i]] - tmp[j];
            self->allpass_buf[i][self->allpass_bufPos[i]] *= allPassFeedBack;
            self->allpass_buf[i][self->allpass_bufPos[i]] += tmp[j];
            self->allpass_bufPos[i]++;
            if (self->allpass_bufPos[i] >= self->allpass_nSamples[i])
                self->allpass_bufPos[i] = 0;
            tmp[j] = x;
        }    
    }
    
    for (i=0; i<self->bufsize; i++) {
        mixtmp = _clip(mix[i]);
        mix1 = sqrtf(mixtmp);
        mix2 = sqrtf(1.0 - mixtmp);
        self->data[i] = (tmp[i] * fixedGain * mix1) + (in[i] * mix2);
    }    
}

static void
Freeverb_transform_aia(Freeverb *self) {
    float x, feedback, damp1, damp2, mix1, mix2, mixtmp;
    int i, j;
    
    float *in = Stream_getData((Stream *)self->input_stream);
    float *siz = Stream_getData((Stream *)self->size_stream);
    float dam = _clip(PyFloat_AS_DOUBLE(self->damp));
    float *mix = Stream_getData((Stream *)self->mix_stream);
    
    damp1 = dam * scaleDamp;
    damp2 = 1.0 - damp1;
    
    float tmp[self->bufsize];
    memset(&tmp, 0, sizeof(tmp));
    
    for (j=0; j<self->bufsize; j++) {
        feedback = _clip(siz[j]) * scaleRoom + offsetRoom;
        for (i=0; i<NUM_COMB; i++) {
            x = self->comb_buf[i][self->comb_bufPos[i]];
            tmp[j] += x;
            self->comb_filterState[i] = (self->comb_filterState[i] * damp1) + (x * damp2);
            x = self->comb_filterState[i] * feedback + in[j];
            self->comb_buf[i][self->comb_bufPos[i]] = x;
            self->comb_bufPos[i]++;
            if (self->comb_bufPos[i] >= self->comb_nSamples[i])
                self->comb_bufPos[i] = 0;
        }
    }
    
    for (i=0; i<NUM_ALLPASS; i++) {
        for (j=0; j<self->bufsize; j++) {
            x = self->allpass_buf[i][self->allpass_bufPos[i]] - tmp[j];
            self->allpass_buf[i][self->allpass_bufPos[i]] *= allPassFeedBack;
            self->allpass_buf[i][self->allpass_bufPos[i]] += tmp[j];
            self->allpass_bufPos[i]++;
            if (self->allpass_bufPos[i] >= self->allpass_nSamples[i])
                self->allpass_bufPos[i] = 0;
            tmp[j] = x;
        }    
    }
    
    for (i=0; i<self->bufsize; i++) {
        mixtmp = _clip(mix[i]);
        mix1 = sqrtf(mixtmp);
        mix2 = sqrtf(1.0 - mixtmp);
        self->data[i] = (tmp[i] * fixedGain * mix1) + (in[i] * mix2);
    }    
}

static void
Freeverb_transform_iaa(Freeverb *self) {
    float x, feedback, damp1, damp2, mix1, mix2, mixtmp;
    int i, j;
    
    float *in = Stream_getData((Stream *)self->input_stream);
    float siz = _clip(PyFloat_AS_DOUBLE(self->size));
    float *dam = Stream_getData((Stream *)self->damp_stream);
    float *mix = Stream_getData((Stream *)self->mix_stream);
    
    feedback = siz * scaleRoom + offsetRoom;
    
    float tmp[self->bufsize];
    memset(&tmp, 0, sizeof(tmp));
    
    for (j=0; j<self->bufsize; j++) {
        damp1 = _clip(dam[j]) * scaleDamp;
        damp2 = 1.0 - damp1;
        for (i=0; i<NUM_COMB; i++) {
            x = self->comb_buf[i][self->comb_bufPos[i]];
            tmp[j] += x;
            self->comb_filterState[i] = (self->comb_filterState[i] * damp1) + (x * damp2);
            x = self->comb_filterState[i] * feedback + in[j];
            self->comb_buf[i][self->comb_bufPos[i]] = x;
            self->comb_bufPos[i]++;
            if (self->comb_bufPos[i] >= self->comb_nSamples[i])
                self->comb_bufPos[i] = 0;
        }
    }
    
    for (i=0; i<NUM_ALLPASS; i++) {
        for (j=0; j<self->bufsize; j++) {
            x = self->allpass_buf[i][self->allpass_bufPos[i]] - tmp[j];
            self->allpass_buf[i][self->allpass_bufPos[i]] *= allPassFeedBack;
            self->allpass_buf[i][self->allpass_bufPos[i]] += tmp[j];
            self->allpass_bufPos[i]++;
            if (self->allpass_bufPos[i] >= self->allpass_nSamples[i])
                self->allpass_bufPos[i] = 0;
            tmp[j] = x;
        }    
    }
    
    for (i=0; i<self->bufsize; i++) {
        mixtmp = _clip(mix[i]);
        mix1 = sqrtf(mixtmp);
        mix2 = sqrtf(1.0 - mixtmp);
        self->data[i] = (tmp[i] * fixedGain * mix1) + (in[i] * mix2);
    }    
}

static void
Freeverb_transform_aaa(Freeverb *self) {
    float x, feedback, damp1, damp2, mix1, mix2, mixtmp;
    int i, j;
    
    float *in = Stream_getData((Stream *)self->input_stream);
    float *siz = Stream_getData((Stream *)self->size_stream);
    float *dam = Stream_getData((Stream *)self->damp_stream);
    float *mix = Stream_getData((Stream *)self->mix_stream);
        
    float tmp[self->bufsize];
    memset(&tmp, 0, sizeof(tmp));
    
    for (j=0; j<self->bufsize; j++) {
        feedback = _clip(siz[j]) * scaleRoom + offsetRoom;
        damp1 = _clip(dam[j]) * scaleDamp;
        damp2 = 1.0 - damp1;
        for (i=0; i<NUM_COMB; i++) {
            x = self->comb_buf[i][self->comb_bufPos[i]];
            tmp[j] += x;
            self->comb_filterState[i] = (self->comb_filterState[i] * damp1) + (x * damp2);
            x = self->comb_filterState[i] * feedback + in[j];
            self->comb_buf[i][self->comb_bufPos[i]] = x;
            self->comb_bufPos[i]++;
            if (self->comb_bufPos[i] >= self->comb_nSamples[i])
                self->comb_bufPos[i] = 0;
        }
    }
    
    for (i=0; i<NUM_ALLPASS; i++) {
        for (j=0; j<self->bufsize; j++) {
            x = self->allpass_buf[i][self->allpass_bufPos[i]] - tmp[j];
            self->allpass_buf[i][self->allpass_bufPos[i]] *= allPassFeedBack;
            self->allpass_buf[i][self->allpass_bufPos[i]] += tmp[j];
            self->allpass_bufPos[i]++;
            if (self->allpass_bufPos[i] >= self->allpass_nSamples[i])
                self->allpass_bufPos[i] = 0;
            tmp[j] = x;
        }    
    }
    
    for (i=0; i<self->bufsize; i++) {
        mixtmp = _clip(mix[i]);
        mix1 = sqrtf(mixtmp);
        mix2 = sqrtf(1.0 - mixtmp);
        self->data[i] = (tmp[i] * fixedGain * mix1) + (in[i] * mix2);
    }    
}

static void Freeverb_postprocessing_ii(Freeverb *self) { POST_PROCESSING_II };
static void Freeverb_postprocessing_ai(Freeverb *self) { POST_PROCESSING_AI };
static void Freeverb_postprocessing_ia(Freeverb *self) { POST_PROCESSING_IA };
static void Freeverb_postprocessing_aa(Freeverb *self) { POST_PROCESSING_AA };
static void Freeverb_postprocessing_ireva(Freeverb *self) { POST_PROCESSING_IREVA };
static void Freeverb_postprocessing_areva(Freeverb *self) { POST_PROCESSING_AREVA };
static void Freeverb_postprocessing_revai(Freeverb *self) { POST_PROCESSING_REVAI };
static void Freeverb_postprocessing_revaa(Freeverb *self) { POST_PROCESSING_REVAA };
static void Freeverb_postprocessing_revareva(Freeverb *self) { POST_PROCESSING_REVAREVA };

static void
Freeverb_setProcMode(Freeverb *self)
{
    int procmode, muladdmode;
    procmode = self->modebuffer[2] + self->modebuffer[3] * 10 + self->modebuffer[4] * 100;
    muladdmode = self->modebuffer[0] + self->modebuffer[1] * 10;

	switch (procmode) {
        case 0:    
            self->proc_func_ptr = Freeverb_transform_iii;
            break;
        case 1:    
            self->proc_func_ptr = Freeverb_transform_aii;
            break;
        case 10:        
            self->proc_func_ptr = Freeverb_transform_iai;
            break;
        case 11:    
            self->proc_func_ptr = Freeverb_transform_aai;
            break;
        case 100:        
            self->proc_func_ptr = Freeverb_transform_iia;
            break;
        case 101:    
            self->proc_func_ptr = Freeverb_transform_aia;
            break;
        case 110:        
            self->proc_func_ptr = Freeverb_transform_iaa;
            break;
        case 111:    
            self->proc_func_ptr = Freeverb_transform_aaa;
            break;
    } 
	switch (muladdmode) {
        case 0:        
            self->muladd_func_ptr = Freeverb_postprocessing_ii;
            break;
        case 1:    
            self->muladd_func_ptr = Freeverb_postprocessing_ai;
            break;
        case 2:    
            self->muladd_func_ptr = Freeverb_postprocessing_revai;
            break;
        case 10:        
            self->muladd_func_ptr = Freeverb_postprocessing_ia;
            break;
        case 11:    
            self->muladd_func_ptr = Freeverb_postprocessing_aa;
            break;
        case 12:    
            self->muladd_func_ptr = Freeverb_postprocessing_revaa;
            break;
        case 20:        
            self->muladd_func_ptr = Freeverb_postprocessing_ireva;
            break;
        case 21:    
            self->muladd_func_ptr = Freeverb_postprocessing_areva;
            break;
        case 22:    
            self->muladd_func_ptr = Freeverb_postprocessing_revareva;
            break;
    }   
}

static void
Freeverb_compute_next_data_frame(Freeverb *self)
{
    (*self->proc_func_ptr)(self); 
    (*self->muladd_func_ptr)(self);
    Stream_setData(self->stream, self->data);
}

static int
Freeverb_traverse(Freeverb *self, visitproc visit, void *arg)
{
    pyo_VISIT
    Py_VISIT(self->input);
    Py_VISIT(self->input_stream);    
    Py_VISIT(self->size);    
    Py_VISIT(self->size_stream);    
    Py_VISIT(self->damp);    
    Py_VISIT(self->damp_stream);    
    Py_VISIT(self->mix);    
    Py_VISIT(self->mix_stream);    
    return 0;
}

static int 
Freeverb_clear(Freeverb *self)
{
    pyo_CLEAR
    Py_CLEAR(self->input);
    Py_CLEAR(self->input_stream);
    Py_CLEAR(self->size);    
    Py_CLEAR(self->size_stream);    
    Py_CLEAR(self->damp);    
    Py_CLEAR(self->damp_stream);    
    Py_CLEAR(self->mix);    
    Py_CLEAR(self->mix_stream);    
    return 0;
}

static void
Freeverb_dealloc(Freeverb* self)
{
    int i;
    free(self->data);
    for(i=0; i<NUM_COMB; i++) {
        free(self->comb_buf[i]);
    }
    for(i=0; i<NUM_ALLPASS; i++) {
        free(self->allpass_buf[i]);
    }    
    Freeverb_clear(self);
    self->ob_type->tp_free((PyObject*)self);
}

static PyObject * Freeverb_deleteStream(Freeverb *self) { DELETE_STREAM };

static PyObject *
Freeverb_new(PyTypeObject *type, PyObject *args, PyObject *kwds)
{
    Freeverb *self;
    self = (Freeverb *)type->tp_alloc(type, 0);

    self->size = PyFloat_FromDouble(.5);
    self->damp = PyFloat_FromDouble(.5);
    self->mix = PyFloat_FromDouble(.5);
	self->modebuffer[0] = 0;
	self->modebuffer[1] = 0;
	self->modebuffer[2] = 0;
	self->modebuffer[3] = 0;
	self->modebuffer[4] = 0;
    
    self->srFactor = pow((DEFAULT_SRATE/self->sr), 0.8);

    INIT_OBJECT_COMMON
    Stream_setFunctionPtr(self->stream, Freeverb_compute_next_data_frame);
    self->mode_func_ptr = Freeverb_setProcMode;
    
    return (PyObject *)self;
}

static int
Freeverb_init(Freeverb *self, PyObject *args, PyObject *kwds)
{
    int i, j, rndSamps;
    float nsamps;
    PyObject *inputtmp, *input_streamtmp, *sizetmp=NULL, *damptmp=NULL, *mixtmp=NULL, *multmp=NULL, *addtmp=NULL;

    static char *kwlist[] = {"input", "size", "damp", "mix", "mul", "add", NULL};

    if (! PyArg_ParseTupleAndKeywords(args, kwds, "O|OOOOO", kwlist, &inputtmp, &sizetmp, &damptmp, &mixtmp, &multmp, &addtmp))
        return -1; 

    Py_XDECREF(self->input);
    self->input = inputtmp;
    input_streamtmp = PyObject_CallMethod((PyObject *)self->input, "_getStream", NULL);
    Py_INCREF(input_streamtmp);
    Py_XDECREF(self->input_stream);
    self->input_stream = (Stream *)input_streamtmp;

    if (sizetmp) {
        PyObject_CallMethod((PyObject *)self, "setSize", "O", sizetmp);
    }

    if (damptmp) {
        PyObject_CallMethod((PyObject *)self, "setDamp", "O", damptmp);
    }

    if (mixtmp) {
        PyObject_CallMethod((PyObject *)self, "setMix", "O", mixtmp);
    }
    
    if (multmp) {
        PyObject_CallMethod((PyObject *)self, "setMul", "O", multmp);
    }

    if (addtmp) {
        PyObject_CallMethod((PyObject *)self, "setAdd", "O", addtmp);
    }
            
    Py_INCREF(self->stream);
    PyObject_CallMethod(self->server, "addStream", "O", self->stream);

    (*self->mode_func_ptr)(self);

    srand((unsigned)(time(0)));
    rndSamps = (rand()/(float)(RAND_MAX) * 20 + 10) / DEFAULT_SRATE;
    for(i=0; i<NUM_COMB; i++) {
        nsamps = Freeverb_calc_nsamples((Freeverb *)self, comb_delays[i] + rndSamps);
        self->comb_buf[i] = (float *)realloc(self->comb_buf[i], (nsamps+1) * sizeof(float));
        self->comb_nSamples[i] = nsamps;
        self->comb_bufPos[i] = 0;
        self->comb_filterState[i] = 0.0;
        for(j=0; j<nsamps; j++) {
            self->comb_buf[i][j] = 0.0;
        }
    }    
        for(i=0; i<NUM_ALLPASS; i++) {
            nsamps = Freeverb_calc_nsamples((Freeverb *)self, allpass_delays[i] + rndSamps);
            self->allpass_buf[i] = (float *)realloc(self->allpass_buf[i], (nsamps+1) * sizeof(float));
            self->allpass_nSamples[i] = nsamps;
            self->allpass_bufPos[i] = 0;
            for(j=0; j<nsamps; j++) {
                self->allpass_buf[i][j] = 0.0;
            }
    }

    Freeverb_compute_next_data_frame((Freeverb *)self);

    Py_INCREF(self);
    return 0;
}

static PyObject * Freeverb_getServer(Freeverb* self) { GET_SERVER };
static PyObject * Freeverb_getStream(Freeverb* self) { GET_STREAM };
static PyObject * Freeverb_setMul(Freeverb *self, PyObject *arg) { SET_MUL };	
static PyObject * Freeverb_setAdd(Freeverb *self, PyObject *arg) { SET_ADD };	
static PyObject * Freeverb_setSub(Freeverb *self, PyObject *arg) { SET_SUB };	
static PyObject * Freeverb_setDiv(Freeverb *self, PyObject *arg) { SET_DIV };	

static PyObject * Freeverb_play(Freeverb *self) { PLAY };
static PyObject * Freeverb_out(Freeverb *self, PyObject *args, PyObject *kwds) { OUT };
static PyObject * Freeverb_stop(Freeverb *self) { STOP };

static PyObject * Freeverb_multiply(Freeverb *self, PyObject *arg) { MULTIPLY };
static PyObject * Freeverb_inplace_multiply(Freeverb *self, PyObject *arg) { INPLACE_MULTIPLY };
static PyObject * Freeverb_add(Freeverb *self, PyObject *arg) { ADD };
static PyObject * Freeverb_inplace_add(Freeverb *self, PyObject *arg) { INPLACE_ADD };
static PyObject * Freeverb_sub(Freeverb *self, PyObject *arg) { SUB };
static PyObject * Freeverb_inplace_sub(Freeverb *self, PyObject *arg) { INPLACE_SUB };
static PyObject * Freeverb_div(Freeverb *self, PyObject *arg) { DIV };
static PyObject * Freeverb_inplace_div(Freeverb *self, PyObject *arg) { INPLACE_DIV };

static PyObject *
Freeverb_setSize(Freeverb *self, PyObject *arg)
{
	PyObject *tmp, *streamtmp;
	
	if (arg == NULL) {
		Py_INCREF(Py_None);
		return Py_None;
	}
    
	int isNumber = PyNumber_Check(arg);
	
	tmp = arg;
	Py_INCREF(tmp);
	Py_DECREF(self->size);
	if (isNumber == 1) {
		self->size = PyNumber_Float(tmp);
        self->modebuffer[2] = 0;
	}
	else {
		self->size = tmp;
        streamtmp = PyObject_CallMethod((PyObject *)self->size, "_getStream", NULL);
        Py_INCREF(streamtmp);
        Py_XDECREF(self->size_stream);
        self->size_stream = (Stream *)streamtmp;
		self->modebuffer[2] = 1;
	}
    
    (*self->mode_func_ptr)(self);
    
	Py_INCREF(Py_None);
	return Py_None;
}	

static PyObject *
Freeverb_setDamp(Freeverb *self, PyObject *arg)
{
	PyObject *tmp, *streamtmp;
	
	if (arg == NULL) {
		Py_INCREF(Py_None);
		return Py_None;
	}
    
	int isNumber = PyNumber_Check(arg);
	
	tmp = arg;
	Py_INCREF(tmp);
	Py_DECREF(self->damp);
	if (isNumber == 1) {
		self->damp = PyNumber_Float(tmp);
        self->modebuffer[3] = 0;
	}
	else {
		self->damp = tmp;
        streamtmp = PyObject_CallMethod((PyObject *)self->damp, "_getStream", NULL);
        Py_INCREF(streamtmp);
        Py_XDECREF(self->damp_stream);
        self->damp_stream = (Stream *)streamtmp;
		self->modebuffer[3] = 1;
	}
    
    (*self->mode_func_ptr)(self);
    
	Py_INCREF(Py_None);
	return Py_None;
}	

static PyObject *
Freeverb_setMix(Freeverb *self, PyObject *arg)
{
	PyObject *tmp, *streamtmp;
	
	if (arg == NULL) {
		Py_INCREF(Py_None);
		return Py_None;
	}
    
	int isNumber = PyNumber_Check(arg);
	
	tmp = arg;
	Py_INCREF(tmp);
	Py_DECREF(self->mix);
	if (isNumber == 1) {
		self->mix = PyNumber_Float(tmp);
        self->modebuffer[4] = 0;
	}
	else {
		self->mix = tmp;
        streamtmp = PyObject_CallMethod((PyObject *)self->mix, "_getStream", NULL);
        Py_INCREF(streamtmp);
        Py_XDECREF(self->mix_stream);
        self->mix_stream = (Stream *)streamtmp;
		self->modebuffer[4] = 1;
	}
    
    (*self->mode_func_ptr)(self);
    
	Py_INCREF(Py_None);
	return Py_None;
}
static PyMemberDef Freeverb_members[] = {
    {"server", T_OBJECT_EX, offsetof(Freeverb, server), 0, "Pyo server."},
    {"stream", T_OBJECT_EX, offsetof(Freeverb, stream), 0, "Stream object."},
    {"input", T_OBJECT_EX, offsetof(Freeverb, input), 0, "Input sound object."},
    {"size", T_OBJECT_EX, offsetof(Freeverb, size), 0, "Room size."},
    {"damp", T_OBJECT_EX, offsetof(Freeverb, damp), 0, "High frequencies damp factor."},
    {"mix", T_OBJECT_EX, offsetof(Freeverb, mix), 0, "Mix between dry and wet signal."},
    {"mul", T_OBJECT_EX, offsetof(Freeverb, mul), 0, "Mul factor."},
    {"add", T_OBJECT_EX, offsetof(Freeverb, add), 0, "Add factor."},
    {NULL}  /* Sentinel */
};

static PyMethodDef Freeverb_methods[] = {
    {"getServer", (PyCFunction)Freeverb_getServer, METH_NOARGS, "Returns server object."},
    {"_getStream", (PyCFunction)Freeverb_getStream, METH_NOARGS, "Returns stream object."},
    {"deleteStream", (PyCFunction)Freeverb_deleteStream, METH_NOARGS, "Remove stream from server and delete the object."},
    {"play", (PyCFunction)Freeverb_play, METH_NOARGS, "Starts computing without sending sound to soundcard."},
    {"out", (PyCFunction)Freeverb_out, METH_VARARGS|METH_KEYWORDS, "Starts computing and sends sound to soundcard channel speficied by argument."},
    {"stop", (PyCFunction)Freeverb_stop, METH_NOARGS, "Stops computing."},
	{"setSize", (PyCFunction)Freeverb_setSize, METH_O, "Sets distortion size factor (0 -> 1)."},
    {"setDamp", (PyCFunction)Freeverb_setDamp, METH_O, "Sets lowpass filter damp factor."},
    {"setMix", (PyCFunction)Freeverb_setMix, METH_O, "Sets the mix factor."},
	{"setMul", (PyCFunction)Freeverb_setMul, METH_O, "Sets oscillator mul factor."},
	{"setAdd", (PyCFunction)Freeverb_setAdd, METH_O, "Sets oscillator add factor."},
    {"setSub", (PyCFunction)Freeverb_setSub, METH_O, "Sets inverse add factor."},
    {"setDiv", (PyCFunction)Freeverb_setDiv, METH_O, "Sets inverse mul factor."},
    {NULL}  /* Sentinel */
};

static PyNumberMethods Freeverb_as_number = {
    (binaryfunc)Freeverb_add,                      /*nb_add*/
    (binaryfunc)Freeverb_sub,                 /*nb_subtract*/
    (binaryfunc)Freeverb_multiply,                 /*nb_multiply*/
    (binaryfunc)Freeverb_div,                   /*nb_divide*/
    0,                /*nb_remainder*/
    0,                   /*nb_divmod*/
    0,                   /*nb_power*/
    0,                  /*nb_neg*/
    0,                /*nb_pos*/
    0,                  /*(unaryfunc)array_abs,*/
    0,                    /*nb_nonzero*/
    0,                    /*nb_invert*/
    0,               /*nb_lshift*/
    0,              /*nb_rshift*/
    0,              /*nb_and*/
    0,              /*nb_xor*/
    0,               /*nb_or*/
    0,                                          /*nb_coerce*/
    0,                       /*nb_int*/
    0,                      /*nb_long*/
    0,                     /*nb_float*/
    0,                       /*nb_oct*/
    0,                       /*nb_hex*/
    (binaryfunc)Freeverb_inplace_add,              /*inplace_add*/
    (binaryfunc)Freeverb_inplace_sub,         /*inplace_subtract*/
    (binaryfunc)Freeverb_inplace_multiply,         /*inplace_multiply*/
    (binaryfunc)Freeverb_inplace_div,           /*inplace_divide*/
    0,        /*inplace_remainder*/
    0,           /*inplace_power*/
    0,       /*inplace_lshift*/
    0,      /*inplace_rshift*/
    0,      /*inplace_and*/
    0,      /*inplace_xor*/
    0,       /*inplace_or*/
    0,             /*nb_floor_divide*/
    0,              /*nb_true_divide*/
    0,     /*nb_inplace_floor_divide*/
    0,      /*nb_inplace_true_divide*/
    0,                     /* nb_index */
};

PyTypeObject FreeverbType = {
    PyObject_HEAD_INIT(NULL)
    0,                         /*ob_size*/
    "_pyo.Freeverb_base",         /*tp_name*/
    sizeof(Freeverb),         /*tp_basicsize*/
    0,                         /*tp_itemsize*/
    (destructor)Freeverb_dealloc, /*tp_dealloc*/
    0,                         /*tp_print*/
    0,                         /*tp_getattr*/
    0,                         /*tp_setattr*/
    0,                         /*tp_compare*/
    0,                         /*tp_repr*/
    &Freeverb_as_number,             /*tp_as_number*/
    0,                         /*tp_as_sequence*/
    0,                         /*tp_as_mapping*/
    0,                         /*tp_hash */
    0,                         /*tp_call*/
    0,                         /*tp_str*/
    0,                         /*tp_getattro*/
    0,                         /*tp_setattro*/
    0,                         /*tp_as_buffer*/
    Py_TPFLAGS_DEFAULT | Py_TPFLAGS_BASETYPE | Py_TPFLAGS_HAVE_GC | Py_TPFLAGS_CHECKTYPES, /*tp_flags*/
    "Freeverb objects. Jezar's Freeverb .",           /* tp_doc */
    (traverseproc)Freeverb_traverse,   /* tp_traverse */
    (inquiry)Freeverb_clear,           /* tp_clear */
    0,		               /* tp_richcompare */
    0,		               /* tp_weaklistoffset */
    0,		               /* tp_iter */
    0,		               /* tp_iternext */
    Freeverb_methods,             /* tp_methods */
    Freeverb_members,             /* tp_members */
    0,                      /* tp_getset */
    0,                         /* tp_base */
    0,                         /* tp_dict */
    0,                         /* tp_descr_get */
    0,                         /* tp_descr_set */
    0,                         /* tp_dictoffset */
    (initproc)Freeverb_init,      /* tp_init */
    0,                         /* tp_alloc */
    Freeverb_new,                 /* tp_new */
};
