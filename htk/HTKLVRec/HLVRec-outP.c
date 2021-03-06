/* ----------------------------------------------------------- */
/*                                                             */
/*                          ___                                */
/*                       |_| | |_/   SPEECH                    */
/*                       | | | | \   RECOGNITION               */
/*                       =========   SOFTWARE                  */ 
/*                                                             */
/*                                                             */
/* ----------------------------------------------------------- */
/* developed at:                                               */
/*                                                             */
/*      Machine Intelligence Laboratory                        */
/*      Department of Engineering                              */
/*      University of Cambridge                                */
/*      http://mi.eng.cam.ac.uk/                               */
/*                                                             */
/* ----------------------------------------------------------- */
/*         Copyright:                                          */
/*         2002-2003  Cambridge University                     */
/*                    Engineering Department                   */
/*                                                             */
/*   Use of this software is governed by a License Agreement   */
/*    ** See the file License for the Conditions of Use  **    */
/*    **     This banner notice must not be removed      **    */
/*                                                             */
/* ----------------------------------------------------------- */
/*         File: HLVRec-outP.c OutP calculation and caching    */
/* ----------------------------------------------------------- */


static void ResetOutPCache (OutPCache *cache)
{
   int i;

   
   if (cache->nStates > 0)
      for (i = 0; i <= cache->nStates; ++i)
         cache->stateT[i] = -1000;
   
   if (cache->nMix > 0)
      for (i = 0; i <= cache->nMix; ++i)
         cache->mixT[i] = -1000;

   cache->cacheHit = cache->cacheMiss = 0;
}

static OutPCache *CreateOutPCache (MemHeap *heap, HMMSet *hset, int block)
{
   OutPCache *cache;

   cache = (OutPCache*) New (heap, sizeof (OutPCache));

   cache->block = block;
   cache->nStates = hset->numSharedStates;
   cache->nMix = hset->numSharedMix;
   /* the sIdx values are 1..numSharedStates, thus the +1 below. Same for mIdx */
   
   cache->stateOutP = cache->mixOutP = NULL;
   if (cache->nStates > 0) {
      cache->stateT = (int *) New (heap, (cache->nStates + 1) * sizeof (int));
      cache->stateOutP = (LogFloat *) New (heap, (cache->nStates + 1) * cache->block * sizeof (LogFloat));
   }
   if (cache->nMix > 0) {
      cache->mixT = (int *) New (heap, (cache->nMix + 1) * sizeof (int));
      cache->mixOutP = (LogFloat *) New (heap, (cache->nMix + 1) * cache->block * sizeof (LogFloat));
   }

   return cache;
}

/* SOutP_ID_mix_Block: returns log prob of stream s of observation x */
LogFloat SOutP_ID_mix_Block(HMMSet *hset, int s, Observation *x, StreamElem *se)
{
   int vSize;
   LogDouble px;
   MixtureElem *me;
   MixPDF *mp;
   Vector v;
   LogFloat wt;

   assert (hset->hsKind == PLAINHS && hset->hsKind == SHAREDHS);
   
   v = x->fv[s];
   vSize = VectorSize(v);
   assert (vSize == hset->swidth[s]);
   me = se->spdf.cpdf+1;
   if (se->nMix == 1){     /* Single Mixture Case */
      mp = me->mpdf; 
      assert (mp->ckind == INVDIAGC);
      /*       px = IDOutP(v,vSize,mp); */
      {
         int i;
         float sum;
         float *mean, *ivar;

         mean = mp->mean;
         ivar = mp->cov.var;

         sum = mp->gConst;
         for (i=1 ; i <= vSize; i++) {
            sum += (v[i] - mean[i]) * (v[i] - mean[i]) * ivar[i];
         }
         px = -0.5*sum;
      }


      return px;
   } else {             /* Multi Mixture Case */
      LogDouble bx = LZERO;                   
      int m;

      for (m=1; m<=se->nMix; m++,me++) {
         wt = MixLogWeight(hset,me->weight);
         if (wt>LMINMIX) {  
            mp = me->mpdf; 
            /*       px = IDOutP(v,vSize,mp);   */
            {
               int i;
               float sum,xmm;
               
               sum = mp->gConst;
               for (i=1;i<=vSize;i++) {
                  xmm = v[i] - mp->mean[i];
                  sum += xmm*xmm*mp->cov.var[i];
               }
               px = -0.5*sum;
            }
            
            bx = LAdd(bx,wt+px);
         }
      }
      return bx;
   }
   return LZERO;;
}

/* cOutP

     caching version of OutP from HModel. This only caches only on a state 
     level, not on a mixture level. 
*/
LogFloat Decoder::cOutP (Observation *x, HLink hmm, int state)
{
   int sIdx, n;
   LogFloat outP;
   OutPCache *cache;

   assert (x == _dec->obsBlock[0]);

   cache = _dec->outPCache;
   sIdx = hmm->svec[state].info->sIdx;

   assert (sIdx >= 0);
   assert (sIdx < cache->nStates);
   
   n = _dec->frame - cache->stateT[sIdx];

   assert (n >= 0);

   if (n < cache->block) {
      outP = cache->stateOutP[sIdx * cache->block + n];
      ++cache->cacheHit;
   }
   else {
      ++cache->cacheMiss;
      if (!cache->mixOutP) {     /* don't bother caching mixtures */
         /* #### handle boundary case where we don't have cache->block obs left */

         if (!_dec->si->useHModel) 
            OutPBlock (_dec->si, &_dec->obsBlock[0], cache->block,
                       sIdx, _dec->acScale, &cache->stateOutP[sIdx * cache->block]);
         else
            OutPBlock_HMod (_dec->si, &_dec->obsBlock[0], cache->block,
                            sIdx, _dec->acScale, &cache->stateOutP[sIdx * cache->block],
                            _dec->frame);
            
         cache->stateT[sIdx] = _dec->frame;
         outP = cache->stateOutP[sIdx * cache->block];
      }
      else {            /* cache mixtures (e.g. for soft-tied systems) */
         abort ();
      }
   }      

   return outP;
}


/* outP caclulation for USEHMODEL=T case  */


/*******************************************************************************/
/*  outP calculation from HModel.c and extended for new adapt code */


static LogFloat SOutP_HMod (HMMSet *hset, int s, Observation *x, StreamElem *se,
                            int id)
{
   int m;
   LogFloat bx,px,wt,det;
   MixtureElem *me;
   MixPDF *mp;
   Vector v,otvs;
   
   /* Note hset->kind == SHAREDHS */
   assert (hset->hsKind == SHAREDHS);

   v=x->fv[s];
   me=se->spdf.cpdf+1;
   if (se->nMix==1){     /* Single Mixture Case */
      bx= MOutP(ApplyCompFXForm(me->mpdf,v,inXForm,&det,id),me->mpdf);
      bx += det;
   } else if (!pde) {
      bx=LZERO;                   /* Multi Mixture Case */
      for (m=1; m<=se->nMix; m++,me++) {
         wt = MixLogWeight(hset,me->weight);
         if (wt>LMINMIX) {   
            px= MOutP(ApplyCompFXForm(me->mpdf,v,inXForm,&det,id),me->mpdf);
            px += det;
            bx=LAdd(bx,wt+px);
         }
      }
   } else {   /* Partial distance elimination */
      wt = MixLogWeight(hset,me->weight);
      mp = me->mpdf;
      otvs = ApplyCompFXForm(mp,v,inXForm,&det,id);
      px = IDOutP(otvs,VectorSize(otvs),mp);
      bx = wt+px+det;
      for (m=2,me=se->spdf.cpdf+2;m<=se->nMix;m++,me++) {
         wt = MixLogWeight(hset,me->weight);
	 if (wt>LMINMIX){
	    mp = me->mpdf;
	    otvs = ApplyCompFXForm(mp,v,inXForm,&det,id);
	    if (PDEMOutP(otvs,mp,&px,bx-wt-det) == TRUE)
	      bx = LAdd(bx,wt+px+det);
	 }
      }
   }
   return bx;
}

LogFloat POutP_HModel (HMMSet *hset,Observation *x, StateInfo *si, int id)
{
   LogFloat bx;
   StreamElem *se;
   Vector w;
   int s,S = x->swidth[0];
   
   if (S==1 && si->weights==NULL)
      return SOutP_HMod(hset,1,x,si->pdf+1, id);
   bx=0.0; se=si->pdf+1; w = si->weights;
   for (s=1;s<=S;s++,se++)
      bx += w[s]*SOutP_HMod(hset,s,x,se, id);
   return bx;
}

void OutPBlock_HMod (StateInfo_lv *si, Observation **obsBlock, 
                int n, int sIdx, float acScale, LogFloat *outP, int id)
{
   int i;

   assert  (si->useHModel);
   
   for (i = 0; i < n; ++i) {
      outP[i] = POutP_HModel (si->hset, obsBlock[i], si->si[sIdx], id);
   }
   
   /* acoustic scaling */
   if (acScale != 1.0)
      for (i = 0; i < n; ++i)
         outP[i] *= acScale;
}
