/*
 * Copyright 2014-2015, Björn Ståhl
 * License: 3-Clause BSD, see COPYING file in arcan source repository.
 * Reference: http://arcan-fe.com
 * Description: Basic pattern matching / transfer statistics / block
 * or sliding data transfers across the arcan shared memory interface.
 */
#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <assert.h>
#include <math.h>

#include <arcan_shmif.h>

#include "rwstat.h"

struct pattern {
	uint8_t* buf;
	size_t buf_sz;
	size_t buf_pos;
	int evc;
	uint8_t alpha;
	uint32_t id;
	enum ptn_flags flags;
};

/* match order for enum rwstat_pack */
static int pack_sizes[] = {
	4,
	3,
	1,
	1
};

struct rwstat_ch_priv {
	enum rwstat_clock clock;
	enum rwstat_pack pack;
	enum rwstat_mapping map;
	enum rwstat_alpha amode;
/* mapping etc. has changed but we havn't told our parent */
	bool status_dirty;

/* scaling factors used for some mapping modes */
	float sf_x, sf_y;
	uint8_t pack_sz;
	uint16_t* cmap;

/* scales O(n) */
	struct pattern* patterns;
	size_t n_patterns;
	size_t patterns_sz;

/* statistics for the data connection as such */
	size_t cnt_total;
	size_t cnt_local;
	size_t buf_ofs;

/* histogram used for estimating entropy etc. */
	uint32_t hgram[256];

/* we need a local intermediary buffer that we flush in
 * order to support switching modes of packing etc. */
	size_t base;
	uint8_t* buf;
	size_t buf_sz;

/* alpha buffer matches base * base and is sampled
 * by the packing function based on the amode of the ch */
	uint8_t* alpha;

/* output segment */
	struct arcan_shmif_cont* cont;
};

/*
 * hilbert curve functions
 * [plucked straight from wikipedia]
 * could traverse the curve in more efficient ways but since
 * it is only used to generate the LUT on resize, don't bother.
 */
static void hilbert_rot(int n, int *x, int *y, int rx, int ry)
{
	if (ry == 0) {
		if (rx == 1) {
			*x = n-1 - *x;
			*y = n-1 - *y;
		}

		int t = *x;
		*x = *y;
		*y = t;
	}
}

static void hilbert_d2xy(int n, int d, int* x, int* y)
{
	int rx, ry, t=d;
	*x = *y = 0;

	for (int s = 1; s < n; s *= 2){
   	rx = 1 & (t / 2);
   	ry = 1 & (t ^ rx);
		hilbert_rot(s, x, y, rx, ry);
		*x += s * rx;
		*y += s * ry;
		t /= 4;
	}
}

static inline void rebuild_hgram(struct rwstat_ch_priv* chp)
{
	for (size_t i = 0; i < chp->buf_sz; i++)
		chp->hgram[ chp->buf[ i ] ]++;
}

/*
 * calculate shannon entropy with a previously supplied histogram
 */
static inline float shent_h(uint8_t* buf, size_t bufsz, uint32_t hgram[256])
{
	float ent = 0.0f;
	for (size_t i = 0; i < bufsz; i++){
		float pr = (float)hgram[buf[i]] / (float) bufsz;
		ent -= pr * log2f(pr);
	}

	return ent;
}

/*
 * calculate shannon entropy without a previous histogram
 */
static inline float shent(uint8_t* buf, size_t bufsz)
{
	uint32_t hgram[256] = {0};
	for (size_t i = 0; i < bufsz; i++)
		hgram[ buf[i] ]++;

	return shent_h(buf, bufsz, hgram);
}

/*
 * normalize histogram to 0..255 range
 */
static inline void hnorm(uint32_t* hgram)
{
	uint32_t acc = 0;
	for (int i = 0; i < 256; i++)
		acc += hgram[ i ];

	if (acc > 0){
		for (int i = 0; i < 256; i++)
			hgram[i] = (uint8_t) (255 * ((float)hgram[i] / (float) acc));
	}
}

static inline void pack_bytes(
	struct rwstat_ch_priv* chp, uint8_t* buf, size_t ofs)
{
	int x = 0, y = 0;
	shmif_pixel val = 0;
	int lofs = 0;

	switch (chp->map){
	case MAP_WRAP:
		x = ofs % chp->base;
		y = ofs / chp->base;
	break;

	case MAP_TUPLE:
		lofs += 2;
		x = (float)buf[0] * chp->sf_x;
		y = (float)buf[1] * chp->sf_y;
	break;

	case MAP_HILBERT:
		x = chp->cmap[ofs * 2 + 0];
		y = chp->cmap[ofs * 2 + 1];
	break;
	}

	switch (chp->pack){
	case PACK_TIGHT:
		val = RGBA(buf[lofs+0], buf[lofs+1], buf[lofs+2], buf[lofs+3]);
	break;
	case PACK_TNOALPHA:
		val = RGBA(buf[lofs+0], buf[lofs+1], buf[lofs+2], chp->alpha[ofs]);
	break;
	case PACK_INTENS:
		val = RGBA(buf[lofs], buf[lofs], buf[lofs], chp->alpha[ofs]);
	break;
	case PACK_HINTENS:
	{
		uint8_t hv = chp->hgram[buf[lofs]];
		val = RGBA(hv, hv, hv, chp->alpha[ofs]);
	}
	break;
	}

	chp->cont->vidp[ chp->cont->addr->w * y + x ] = val;
}

/*
 * build alphamap with shannon entropy based on a specific blocksize,
 * bsz should always be % chp->buf_sz otherwise
 */
static void update_entalpha(struct rwstat_ch_priv* chp, size_t bsz)
{
	size_t bsqr = chp->base * chp->base;

	for (size_t i = 0; i < bsqr; i += bsz){
		uint8_t entalpha = (uint8_t) (255.0f *
			(shent(&chp->buf[i*chp->pack_sz], bsz * chp->pack_sz) / 8.0f));

		memset(&chp->alpha[i], entalpha, bsz);
	}
}

/*
 * Use the current set of patterns to populate the alpha buffer
 * that is then sampled when building the final output.
 */
static void update_ptnalpha(struct rwstat_ch_priv* chp)
{
	uint8_t av = 0xff;
	if (chp->n_patterns == 0){
		memset(chp->alpha, av, chp->base * chp->base);
		return;
	}

/* reset patterns */
	for (size_t i = 0; i < chp->n_patterns; i++){
		chp->patterns[i].buf_pos = 0;
		chp->patterns[i].evc = 0;
	}

/* If ptn-match ever becomes a performance choke,
 * here is a good spot for adding parallelization. */
	for (size_t i = 0; i < chp->buf_sz; i++){
		chp->alpha[i] = av;

		for (size_t j = 0; j < chp->n_patterns; j++){
			struct pattern* ptn = &chp->patterns[j];
			if (ptn->buf[ptn->buf_pos] == chp->buf[i])
			 	if (++(ptn->buf_pos) == ptn->buf_sz){
					chp->patterns[j].buf_pos = 0;
					memset(&chp->alpha[i - ptn->buf_sz], ptn->alpha, ptn->buf_sz);
					if ((ptn->flags & FLAG_STATE))
						av = ptn->alpha;
					if ((ptn->flags & FLAG_EVENT))
						ptn->evc++;
				}
			}

	}


/* Check matched patterns and fire an event with the matching
 * identifier, and the number of times each event was matched
 * in the buffer window. Abuse the CURSORINPUT event for this */
	for (size_t i = 0; i < chp->n_patterns; i++)
		if (chp->patterns[i].evc){
			arcan_event ev = {
				.category = EVENT_EXTERNAL,
				.ext.kind = EVENT_EXTERNAL_CURSORINPUT,
				.ext.cursor.id = chp->patterns[i].id,
				.ext.cursor.x = chp->patterns[i].evc
			};
			arcan_shmif_enqueue(chp->cont, &ev);
			chp->patterns[i].evc = 0;
		}
}

/*
 * Build the output buffer and push/synch to an external recipient,
 * taking mapping function, alpha population functions, and timing-
 * related metadata.
 */
static void ch_step(struct rwstat_ch* ch)
{
	struct rwstat_ch_priv* chp = ch->priv;

	struct arcan_event outev = {
		.category = EVENT_EXTERNAL,
		.ext.kind = EVENT_EXTERNAL_FRAMESTATUS,
		.ext.framestatus.framenumber = ch->priv->cnt_local,
		.ext.framestatus.pts = ch->priv->cnt_total,
		.ext.framestatus.acquired = arcan_timemillis(),
	};

	size_t ntw = chp->base * chp->base;
	outev.ext.framestatus.fhint = shent_h(chp->buf,
			chp->buf_sz, chp->hgram) / 8.0;
	ch->event(ch, &outev);

/*
 * Notify about the packing mode active for this frame. This is
 * needed for the parent to be able to determine what each byte
 * corresponds to.
 */
	if (chp->status_dirty){
		outev.ext.kind = EVENT_EXTERNAL_STREAMINFO;
		outev.ext.streaminf.streamid = 0;
		outev.ext.streaminf.datakind = 0;
		outev.ext.streaminf.langid[0] = '0' + chp->pack;
		outev.ext.streaminf.langid[1] = '0' + chp->map;
		outev.ext.streaminf.langid[2] = '0' + chp->pack_sz;
		chp->status_dirty = false;
		ch->event(ch, &outev);
	}

	if ( 1 == (chp->clock & (RW_CLK_SLIDE)) )
		rebuild_hgram(chp);

	if (chp->pack == PACK_HINTENS)
		hnorm(chp->hgram);
	if (chp->amode == RW_ALPHA_ENTBASE)
		update_entalpha(chp, chp->base);
	else if (chp->amode == RW_ALPHA_PTN)
		update_ptnalpha(chp);

	for (size_t i = 0; i < chp->buf_sz; i+= chp->pack_sz)
		pack_bytes(chp, &chp->buf[i], i / chp->pack_sz);

	arcan_shmif_signal(chp->cont, SHMIF_SIGVID);
	chp->cnt_local = chp->cnt_total;

/* non-sparse mappings require an output flush */
	if (chp->map == MAP_TUPLE){
		shmif_pixel val = RGBA(0x00, 0x00, 0x00, 0xff);
		for (size_t i = 0; i < ntw; i++)
			chp->cont->vidp[i] = val;
	}
}

static void ch_event(struct rwstat_ch* ch, arcan_event* ev)
{
	arcan_shmif_enqueue(ch->priv->cont, ev);
}

static size_t ch_data(struct rwstat_ch* ch,
	uint8_t* buf, size_t buf_sz, int* step)
{
	struct rwstat_ch_priv* chp = ch->priv;
	size_t ntw;

/* larger write chunks are equivalent to a block slide,
 * so use the CLK_BLOCK mode for those */
	if (ch->priv->clock == RW_CLK_SLIDE){
		if (buf_sz < chp->buf_sz){
			ntw = buf_sz;
			chp->buf_ofs = chp->buf_sz - ntw;
			memmove(chp->buf, chp->buf + ntw, chp->buf_ofs);
		}
		else
			ntw = chp->buf_sz;
	}
	else
		ntw = buf_sz < (chp->buf_sz - chp->buf_ofs) ?
			buf_sz : chp->buf_sz - chp->buf_ofs;

/* add to remap buffer and histogram,
 * histogram need to be rebuilt for CLK_SLIDE but add > branch */
	for (size_t i = 0; i < ntw; i++){
		chp->hgram[ buf[i] ]++;
		chp->buf[ chp->buf_ofs++ ] = buf[i];
	}

	if (chp->buf_ofs == chp->buf_sz){
		chp->buf_ofs = 0;
		*step = 1;
		ch_step(ch);
	}
	else
		*step = 0;

	return ntw;
}

static bool ch_pattern(struct rwstat_ch* ch,
	uint8_t alpha, uint32_t id, enum ptn_flags fl, void* buf, size_t buf_sz)
{
	struct rwstat_ch_priv* chp = ch->priv;
	if (chp->n_patterns+1 > chp->patterns_sz){
		void* rbuf = realloc(chp->patterns,
			(chp->patterns_sz + 8) * sizeof(struct pattern));

		if (!rbuf){
			free(buf);
			return false;
		}

		chp->patterns = rbuf;
		memset(&chp->patterns[chp->n_patterns], '\0', sizeof(struct pattern) * 8);
		chp->patterns_sz += 8;
	}

	struct pattern* newp = &chp->patterns[chp->n_patterns++];

	newp->buf = buf;
	newp->buf_sz = buf_sz;
	newp->alpha = alpha;
	newp->id = id;
	newp->flags = fl;

	return true;
}

static void ch_tick(struct rwstat_ch* ch)
{
	ch_step(ch);
}

struct arcan_shmif_cont* ch_context(struct rwstat_ch* ch)
{
	return ch->priv->cont;
}

static void ch_pack(struct rwstat_ch* ch, enum rwstat_pack pack)
{
	struct rwstat_ch_priv* chp = ch->priv;
	chp->pack = pack;

/* number of bytes we need to fill one shmif_pixel */
	chp->pack_sz = pack_sizes[pack];

/* then number of bytes we need in order to map coordinates */
	switch(ch->priv->map){
	case MAP_WRAP: break; /* can use the buf_ofs value for this */
	case MAP_TUPLE: chp->pack_sz += 2; break;
	case MAP_HILBERT: break; /* can use the buf_ofs value for this */
	}

/* since packing size might have changed, we need to do a sanity check */
	if (chp->buf_sz != (chp->base * chp->base) * chp->pack_sz)
		ch->resize(ch, chp->base);

	chp->status_dirty = true;
}

static void ch_map(struct rwstat_ch* ch, enum rwstat_mapping map)
{
	struct rwstat_ch_priv* chp = ch->priv;
	chp->map = map;

	if (chp->cmap){
		free(chp->cmap);
		chp->cmap = NULL;
	}

	size_t hsz = ch->priv->base * ch->priv->base;

/* some mapping modes need a LUT for the ofs = F(X,Y) */
	if (map == MAP_WRAP || map == MAP_TUPLE)
		;
	else if (map == MAP_HILBERT){
		uint16_t* cmap = malloc( 2 * 2 * hsz );
		for (size_t i = 0; i < hsz; i++){
			int x, y;
			hilbert_d2xy(ch->priv->base, i, &x, &y);
			cmap[i * 2 + 0] = x;
			cmap[i * 2 + 1] = y;
		}
		chp->cmap = cmap;
	}

/* changing mapping mode may require different packing dimensions */
	ch_pack(ch, chp->pack);

/* reset the buffer to reflect change in mapping mode, this doesn't
 * matter in CLK_BYTES but for other modes */
	if (map == MAP_TUPLE){
		shmif_pixel val = RGBA(0x00, 0x00, 0x00, 0xff);
		size_t ntw = chp->base * chp->base;
		for (size_t i = 0; i < ntw; i++)
			chp->cont->vidp[i] = val;
	}

	chp->status_dirty = true;
	ch_step(ch);
}

static void ch_alpha(struct rwstat_ch* ch, enum rwstat_alpha amode)
{
	ch->priv->amode = amode;
	if (0 == amode)
		memset(ch->priv->alpha, 0xff, ch->priv->base * ch->priv->base);
}

static void ch_reclock(struct rwstat_ch* ch, enum rwstat_clock clock)
{
/*	ch_step(ch); - somewhat uncertain if there is any valid point
 *	in enforcing a step on the change of clocking function */
	ch->priv->clock = clock;
}

static size_t ch_rowsz(struct rwstat_ch* ch)
{
	return ch->priv->pack_sz * ch->priv->cont->addr->w;
}

static void ch_free(struct rwstat_ch** ch)
{
	struct rwstat_ch_priv* chp = (*ch)->priv;
	for (size_t i = 0; i < chp->patterns_sz; i++){
		free(chp->patterns[i].buf);
	}
	free(chp->patterns);

	memset((*ch)->priv, '\0', sizeof(struct rwstat_ch_priv));
	free((*ch)->priv);
	memset(*ch, '\0', sizeof(struct rwstat_ch));
	free(*ch);
	*ch = NULL;
}

static size_t ch_left(struct rwstat_ch* ch)
{
	return ch->priv->buf_sz - ch->priv->buf_ofs;
}

static void ch_resize(struct rwstat_ch* ch, size_t base)
{
/* initial state, black! */
	if (ch->priv->buf){
		free(ch->priv->buf);
		free(ch->priv->alpha);
	}

/*
 * It is possible to change mapping without elaborate sliding buffer
 * windows (some mappings will only be more sparse), but we cannot do
 * the same with packing. Thus, the current packing mode dictates
 * how large the raw buffer needs to be.
 */
	size_t bsqr = base * base;
	ch->priv->buf_sz = bsqr * ch->priv->pack_sz;
	ch->priv->buf = malloc(ch->priv->buf_sz);
	ch->priv->alpha = malloc(bsqr);

	memset(ch->priv->buf, '\0', ch->priv->buf_sz);
	memset(ch->priv->alpha, 0xff, bsqr);
	ch->priv->base = base;
	ch->priv->sf_x = (float) (base-1) / 255.0f;
	ch->priv->sf_y = (float) (base-1) / 255.0f;

/* will setup / rebuild LUTs etc. */
	ch_map(ch, ch->priv->map);
}

static void ch_wind(struct rwstat_ch* ch, off_t ofs)
{
	ch->priv->cnt_total = ofs;
}

bool rwstat_consume_event(struct rwstat_ch* ch, struct arcan_event* ev)
{
	if (ev->category != EVENT_TARGET || ev->tgt.kind != TARGET_COMMAND_GRAPHMODE)
		return false;

	switch (ev->tgt.ioevs[0].iv){
	case 0:
		ch->switch_clock(ch, RW_CLK_BLOCK);
	break;
	case 1:
	 	ch->switch_clock(ch, RW_CLK_SLIDE);
	break;
	case 10:
		ch->switch_mapping(ch, MAP_WRAP);
	break;
	case 11:
		ch->switch_mapping(ch, MAP_TUPLE);
	break;
	case 12:
		ch->switch_mapping(ch, MAP_HILBERT);
	break;
	case 20:
		ch->switch_packing(ch, PACK_INTENS);
	break;
	case 21:
		ch->switch_packing(ch, PACK_HINTENS);
	break;
	case 22:
		ch->switch_packing(ch, PACK_TIGHT);
	break;
	case 23:
		ch->switch_packing(ch, PACK_TNOALPHA);
	break;
	case 30:
		ch->switch_alpha(ch, RW_ALPHA_FULL);
	break;
	case 31:
		ch->switch_alpha(ch, RW_ALPHA_PTN);
	break;
	case 32:
		ch->switch_alpha(ch, RW_ALPHA_ENTBASE);
	break;
	default:
		fprintf(stderr, "Senseye:FDsense:dispatch_event(),"
			" unknown graphmode: %d\n", ev->tgt.ioevs[0].iv);
		return false;
	}

	return true;
}

/*
 * Each pattern is 8-bit unsigned represented as hexadecimal in ascii.
 * This feature should be expanded to something slightly more flexible
 * (without turning into a turing-complete monster, for that we might
 * as well have a pipe() + fork()/exec()+seccmp() based plugin system):
 *
 *  - type- defined context
 *  - specific encoding (u16,32,float,utf,ucs...) matching
 *  - length field
 *  - padding
 *  - reset-counter (after detected pattern, wait n bytes)
 *  - injection point / trigger (in-place replace data)
 *  - enable / disable other pattern on activation
 *
 * The big caveat is that these are constrained and reset to each
 * synched buffer transfer, as the number of edge conditions when
 * taking seeking/stepping/clock modes etc. into account is a bit
 * too much.
 */
void rwstat_addpatterns(struct rwstat_ch* ch, struct arg_arr* arg)
{
	int ind = 0;
	const char* val;

	if (!arg)
		return;

	while (arg_lookup(arg, "val", ind, &val)){
		size_t count = 0;
		char* work = strdup(val);
		char* nptr;
		char* tv = strtok(work, ",");
		if (!tv){
			free(work);
			return;
		}

/* pass 1, count */
		while (tv){
			uint8_t bytev = strtoul(tv, &nptr, 16);
			if (bytev == 0 && nptr == NULL){
				free(work);
				return;
			}

			count++;
			tv = strtok(NULL, ",");
		}

/* pass 2, setup */
		free(work);
		work = strdup(val);
		tv = strtok(work, ",");
		uint8_t* bptn = malloc(sizeof(uint8_t) * count);
		count = 0;
		while (tv){
			uint8_t bytev = strtoul(tv, &nptr, 16);
			bptn[count++] = bytev;
			tv = strtok(NULL, ",");
		}
		free(work);

		if (!arg_lookup(arg, "opt", ind, &val))
			return;

/* extract meta from opt, i.e. id, alpha, state */
		ch_pattern(ch, ind, ind, 0, bptn, count);
		ind++;
	}
}

struct rwstat_ch* rwstat_addch(
	enum rwstat_clock mode, enum rwstat_mapping map, enum rwstat_pack pack,
 	struct arcan_shmif_cont* c)
{
	if (!c)
		return NULL;

	struct rwstat_ch* res;
	res = malloc(sizeof(struct rwstat_ch));
	memset(res, '\0', sizeof(struct rwstat_ch));
	res->priv = malloc(sizeof(struct rwstat_ch_priv));
	memset(res->priv, '\0', sizeof(struct rwstat_ch_priv));

	res->priv->cont = c;
	res->data = ch_data;
	res->priv->clock = mode;
	res->switch_packing = ch_pack;
	res->switch_mapping = ch_map;
	res->switch_clock = ch_reclock;
	res->switch_alpha = ch_alpha;
	res->tick = ch_tick;
	res->context = ch_context;
	res->free = ch_free;
	res->event = ch_event;
	res->wind_ofs = ch_wind;
	res->resize = ch_resize;
	res->add_pattern = ch_pattern;
	res->left = ch_left;
	res->row_size = ch_rowsz;

	res->priv->map = map;
	res->priv->pack = pack;
	res->priv->amode = RW_ALPHA_ENTBASE;
	res->resize(res, c->addr->w);
	res->priv->status_dirty = true;

	return res;
}
