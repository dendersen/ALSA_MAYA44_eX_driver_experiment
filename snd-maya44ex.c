// SPDX-License-Identifier: GPL-2.0
/*
 * ALSA driver for the ESI MAYA44 eX (PCI 19fe:7000, ES7010L bridge, 2x WM8776)
 *
 * Structure follows "Writing an ALSA Driver": a per-card chip struct held in
 * card->private_data, a chip create/free pair, separate PCM and mixer
 * components, a PCI probe/remove pair and PM suspend/resume callbacks.
 *
 * The register map is derived from traces of the vendor Windows driver; there
 * is no vendor documentation. Hardware model:
 *
 *  - Four DMA engines (n = 0..3, block 0x80 + 0x20 * n), each a stereo stream
 *    of 32-bit words (24-bit left-justified) in a 16 KiB ring of two 8 KiB
 *    periods, described by a scatter/gather table.
 *  - Engines 2/3 are outputs 1/2 and 3/4; engines 0/1 are inputs 1/2 and 3/4
 *    (input mapping inferred).
 *  - The engines start and stop as one group and share one sample clock. Only
 *    engine 2 raises the period interrupt.
 *  - Two WM8776 codecs (bus addresses 0x34 and 0x36) sit behind a serial
 *    master in the ES7010L.
 */

#include <linux/module.h>
#include <linux/pci.h>
#include <linux/interrupt.h>
#include <linux/delay.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/mutex.h>
#include <linux/slab.h>
#include <linux/uio.h>
#include <sound/core.h>
#include <sound/pcm.h>
#include <sound/control.h>
#include <sound/tlv.h>
#include <sound/initval.h>

MODULE_AUTHOR("PCI_Pass lab");
MODULE_DESCRIPTION("ESI MAYA44 eX (ES7010L) ALSA driver");
MODULE_LICENSE("GPL");

static int index[SNDRV_CARDS] = SNDRV_DEFAULT_IDX;
static char *id[SNDRV_CARDS] = SNDRV_DEFAULT_STR;
static bool enable[SNDRV_CARDS] = SNDRV_DEFAULT_ENABLE_PNP;
static bool unverified_rates;

module_param_array(index, int, NULL, 0444);
MODULE_PARM_DESC(index, "Index value for the MAYA44 eX card.");
module_param_array(id, charp, NULL, 0444);
MODULE_PARM_DESC(id, "ID string for the MAYA44 eX card.");
module_param_array(enable, bool, NULL, 0444);
MODULE_PARM_DESC(enable, "Enable the MAYA44 eX card.");
module_param(unverified_rates, bool, 0444);
MODULE_PARM_DESC(unverified_rates,
		 "Also offer 32/48 kHz (clock words not proven on hardware).");

#define PCI_VENDOR_ID_ESI	0x19fe
#define PCI_DEVICE_ID_MAYA44EX	0x7000

/* Global registers (BAR 0) */
#define REG_RESET		0x008
#define REG_SHARED_TABLE	0x070
#define REG_RATE_CLK(n)		(0x092 + 0x20 * (n))	/* 16 bit */
#define REG_GPIO_A		0x188
#define REG_GPIO_B		0x189
#define REG_CLOCK_SEL		0x1a0
#define REG_CLOCK_STAT		0x1b0
#define REG_DMA_GLOBAL		0x404
#define REG_DMA_GLOBAL2		0x40c
#define REG_EEPROM		0x460	/* 0x460..0x464 command, 0x474 data */
#define REG_EEPROM_DATA		0x474
#define REG_ROUTE_A		0x480
#define REG_ROUTE		0x481
#define REG_IRQ_CAUSE		0x4b0
#define REG_I2C_ADDR		0x4c0
#define REG_I2C_BYTE0		0x4c1
#define REG_I2C_PAD		0x4c2
#define REG_I2C_COUNT		0x4c3
#define REG_I2C_CTRL		0x4c4
#define REG_I2C_DATA		0x4d0
#define REG_CLK_BB		0x4e0
#define REG_CLK_BB_CTRL		0x4e4

#define I2C_DONE		BIT(31)
#define I2C_BUSY		BIT(7)
#define EEPROM_READY		BIT(30)

/* Engine registers, relative to the engine block */
#define ENG_BASE(n)		(0x80 + 0x20 * (n))
#define ENG_CTRL		0x00
#define ENG_STATUS		0x03
#define ENG_POS			0x04
#define ENG_SIZE		0x08
#define ENG_ENABLE		0x0c
#define ENG_CHAN		0x0e
#define ENG_CFG			0x10
#define ENG_SG_LO		0x18
#define ENG_SG_HI		0x1c
#define ENG_PERIOD(n)		(0x190 + 4 * (n))
#define ENG_STATE(n)		(0x192 + 4 * (n))

#define NUM_ENGINES		4
#define NUM_PAIRS		2
#define ENG_IN12		0
#define ENG_OUT12		2
#define IRQ_ENGINE		ENG_OUT12

#define RING_BYTES		0x4000
#define PERIOD_BYTES		0x2000
#define FRAME_BYTES		8		/* stereo, 32 bit per sample */
#define RING_FRAMES		(RING_BYTES / FRAME_BYTES)
#define PERIOD_FRAMES		1024
#define SG_STRIDE		0x100
#define SHARED_OFFSET		0xa00
#define TABLE_AREA_BYTES	0x1000

/* Sample-rate words; only 44.1 kHz is proven on real hardware */
struct maya_rate {
	unsigned int rate;
	u16 clk;
	u32 sel;
	bool verified;
};

static const struct maya_rate maya_rates[] = {
	{ 44100, 0x0031, 0x2000, true },
	{ 32000, 0x0a31, 0x3000, false },
	{ 48000, 0x0831, 0xa000, false },
};

static const unsigned int maya_rates_verified[] = { 44100 };
static const unsigned int maya_rates_all[] = { 32000, 44100, 48000 };

static const struct snd_pcm_hw_constraint_list maya_constraint_verified = {
	.count = ARRAY_SIZE(maya_rates_verified),
	.list = maya_rates_verified,
};

static const struct snd_pcm_hw_constraint_list maya_constraint_all = {
	.count = ARRAY_SIZE(maya_rates_all),
	.list = maya_rates_all,
};

/* WM8776 */
#define WM_DAC_ATTEN_L		0x03
#define WM_DAC_ATTEN_R		0x04
#define WM_DAC_MUTE		0x08
#define WM_ADC_GAIN_L		0x0e
#define WM_ADC_GAIN_R		0x0f
#define WM_NUM_REGS		0x17
#define WM_UPDATE		0x100

static const u8 maya_codec_addr[2] = { 0x34, 0x36 };

/* Power-on table replayed from the vendor driver: { register, 9-bit value } */
static const u16 maya_codec_init[][2] = {
	{ 0x00, 0x1f9 }, { 0x01, 0x1f9 }, { 0x02, 0x180 }, { 0x03, 0x100 },
	{ 0x04, 0x100 }, { 0x05, 0x1ff }, { 0x06, 0x000 }, { 0x07, 0x095 },
	{ 0x08, 0x000 }, { 0x09, 0x000 }, { 0x0a, 0x022 }, { 0x0b, 0x022 },
	{ 0x0c, 0x022 }, { 0x0d, 0x000 }, { 0x0e, 0x1cf }, { 0x0f, 0x1cf },
	{ 0x10, 0x07a }, { 0x11, 0x000 }, { 0x12, 0x032 }, { 0x13, 0x000 },
	{ 0x14, 0x0bc }, { 0x15, 0x002 }, { 0x16, 0x001 },
	/* un-attenuate the DACs and set the ADC gain to 0 dB */
	{ 0x03, 0x1ff }, { 0x04, 0x1ff }, { 0x02, 0x100 },
};

struct maya_stream {
	struct snd_pcm_substream *substream;
	bool running;
	unsigned int base;	/* ring frame that corresponds to ALSA frame 0 */
};

struct maya44ex {
	struct snd_card *card;
	struct pci_dev *pci;
	void __iomem *iobase;
	int irq;

	spinlock_t lock;		/* stream state, engine control */
	struct mutex codec_lock;	/* codec bus and register cache */

	struct snd_dma_buffer ring[NUM_ENGINES];
	struct snd_dma_buffer tables;
	u8 *scratch;

	struct snd_pcm *pcm[NUM_PAIRS];
	struct maya_stream stream[NUM_PAIRS][2];	/* [pair][direction] */
	bool group_running;
	unsigned int rate;
	unsigned int nrunning;

	u16 codec_reg[2][WM_NUM_REGS];
};

static inline u32 maya_r32(struct maya44ex *c, u32 off)
{
	return readl(c->iobase + off);
}

static inline void maya_w32(struct maya44ex *c, u32 off, u32 v)
{
	writel(v, c->iobase + off);
}

static inline void maya_w16(struct maya44ex *c, u32 off, u16 v)
{
	writew(v, c->iobase + off);
}

static inline u8 maya_r8(struct maya44ex *c, u32 off)
{
	return readb(c->iobase + off);
}

static inline void maya_w8(struct maya44ex *c, u32 off, u8 v)
{
	writeb(v, c->iobase + off);
}

/*
 * Serial EEPROM
 */

/* The vendor driver reads 4 bytes at start-up: 51 0e 03 00 (= 0e51:0003) */
static int maya_eeprom_read(struct maya44ex *chip, u8 addr, u8 *out)
{
	u32 st;

	maya_w8(chip, REG_EEPROM, 0xa0);
	maya_w8(chip, REG_EEPROM + 1, addr);
	maya_w8(chip, REG_EEPROM + 2, 0);
	maya_w8(chip, REG_EEPROM + 3, 1);
	maya_w32(chip, REG_EEPROM + 4, 0xe0000450);
	maya_w8(chip, REG_EEPROM + 4, 0xd0);
	if (readl_poll_timeout(chip->iobase + REG_EEPROM + 4, st,
			       st == U32_MAX ||
			       ((st & EEPROM_READY) && !(st & I2C_BUSY)),
			       10, 50000) || st == U32_MAX)
		return -EIO;
	*out = maya_r8(chip, REG_EEPROM_DATA);
	return 0;
}

/*
 * Codec access
 */

static int maya_i2c_write(struct maya44ex *chip, u8 addr, u8 byte0, u8 data)
{
	u32 st = 0;
	int try;

	for (try = 0; try < 3; try++) {
		maya_w8(chip, REG_I2C_ADDR, addr);
		maya_w8(chip, REG_I2C_BYTE0, byte0);
		maya_w8(chip, REG_I2C_PAD, 0);
		maya_w8(chip, REG_I2C_COUNT, 1);
		maya_w32(chip, REG_I2C_CTRL, 0xe0000010);
		maya_w8(chip, REG_I2C_DATA, data);
		maya_w8(chip, REG_I2C_CTRL, 0x90);

		if (!readl_poll_timeout(chip->iobase + REG_I2C_CTRL, st,
					st == U32_MAX ||
					((st & I2C_DONE) && !(st & I2C_BUSY)),
					10, 5000)) {
			if (st == U32_MAX)
				return -EIO;
			/* the next transfer fails if started too early */
			usleep_range(200, 400);
			return 0;
		}
	}
	dev_err(chip->card->dev,
		"codec 0x%02x reg 0x%02x write timed out (0x4c4=0x%08x)\n",
		addr, byte0 >> 1, st);
	return -EIO;
}

/* Must be called with codec_lock held (or during init/resume) */
static int maya_codec_write(struct maya44ex *chip, int codec, u8 reg, u16 val)
{
	int err;

	/* two-wire frame: 7-bit register, 9-bit value, MSB in the first byte */
	err = maya_i2c_write(chip, maya_codec_addr[codec],
			     (reg << 1) | ((val >> 8) & 1), val & 0xff);
	if (!err)
		chip->codec_reg[codec][reg] = val & 0x1ff;
	return err;
}

/*
 * DMA engine group
 */

static void maya_engine_setup(struct maya44ex *chip, int n)
{
	u32 b = ENG_BASE(n);
	__le32 *sg = (__le32 *)(chip->tables.area + SG_STRIDE * (n + 1));
	dma_addr_t sg_addr = chip->tables.addr + SG_STRIDE * (n + 1);
	int i;

	memset(sg, 0, SG_STRIDE);
	for (i = 0; i < RING_BYTES / PERIOD_BYTES; i++) {
		dma_addr_t a = chip->ring[n].addr + i * PERIOD_BYTES;

		sg[i * 4 + 0] = cpu_to_le32(lower_32_bits(a));
		sg[i * 4 + 2] = cpu_to_le32(PERIOD_BYTES);
	}
	wmb();

	/* reset pulse */
	maya_w32(chip, b + ENG_CTRL, 0xff000000);
	maya_w8(chip, ENG_STATE(n), 2);
	maya_w8(chip, b + ENG_CTRL, 1);
	maya_r32(chip, b + ENG_CTRL);
	maya_w8(chip, b + ENG_CTRL, 0);

	maya_w32(chip, b + ENG_SG_LO, lower_32_bits(sg_addr));
	maya_w32(chip, b + ENG_SG_HI, 0);
	maya_w16(chip, b + ENG_CFG, n >= ENG_OUT12 ? 0xbf : 0x77);
	maya_w16(chip, b + ENG_CHAN, 4);
	maya_w8(chip, b + ENG_ENABLE, 1);
	maya_w32(chip, b + ENG_SIZE, RING_BYTES);
	maya_w16(chip, ENG_PERIOD(n), PERIOD_FRAMES);
}

static void maya_engine_start(struct maya44ex *chip, int n)
{
	u32 b = ENG_BASE(n);

	maya_w8(chip, b + ENG_STATUS, 0xff);
	maya_w8(chip, ENG_STATE(n), n == IRQ_ENGINE ? 7 : 3);
	maya_r32(chip, b + ENG_CTRL);
	maya_w8(chip, b + ENG_CTRL, 2);
}

static int maya_set_rate(struct maya44ex *chip, unsigned int rate)
{
	int i, n;

	for (i = 0; i < ARRAY_SIZE(maya_rates); i++)
		if (maya_rates[i].rate == rate &&
		    (maya_rates[i].verified || unverified_rates))
			break;
	if (i == ARRAY_SIZE(maya_rates))
		return -EINVAL;

	for (n = 0; n < NUM_ENGINES; n++)
		maya_w16(chip, REG_RATE_CLK(n), maya_rates[i].clk);
	maya_w8(chip, REG_ROUTE, 0x2a);
	maya_w32(chip, REG_CLOCK_SEL, 0x80000000 | maya_rates[i].sel);
	/* write-1-to-clear */
	maya_w32(chip, REG_CLOCK_STAT, maya_r32(chip, REG_CLOCK_STAT));
	return 0;
}

/* Called with chip->lock held */
static int maya_group_start(struct maya44ex *chip)
{
	static const int setup_order[NUM_ENGINES] = { 2, 3, 0, 1 };
	static const int start_order[NUM_ENGINES] = { 1, 0, 3, 2 };
	int i, err;

	maya_w32(chip, REG_DMA_GLOBAL, 0x50078000);
	maya_w16(chip, REG_DMA_GLOBAL2, 0x301);
	maya_w8(chip, REG_DMA_GLOBAL, 0x0f);
	/* the hardware changes the low bits; the vendor driver writes them back */
	maya_w32(chip, REG_DMA_GLOBAL, maya_r32(chip, REG_DMA_GLOBAL));

	memset(chip->tables.area + SHARED_OFFSET, 0, SG_STRIDE);
	maya_w32(chip, REG_SHARED_TABLE,
		 lower_32_bits(chip->tables.addr + SHARED_OFFSET));

	for (i = 0; i < NUM_ENGINES; i++)
		maya_engine_setup(chip, setup_order[i]);

	err = maya_set_rate(chip, chip->rate);
	if (err)
		return err;

	for (i = 0; i < NUM_ENGINES; i++)
		maya_engine_start(chip, start_order[i]);

	chip->group_running = true;
	return 0;
}

/* Called with chip->lock held */
static void maya_group_stop(struct maya44ex *chip)
{
	int n;

	if (!chip->group_running)
		return;
	for (n = 0; n < NUM_ENGINES; n++) {
		u32 b = ENG_BASE(n);

		maya_w8(chip, b + ENG_CTRL, 1);
		maya_r32(chip, b + ENG_CTRL);
		maya_w8(chip, b + ENG_CTRL, 0);
		maya_w8(chip, ENG_STATE(n), 2);
	}
	maya_w32(chip, REG_CLOCK_SEL, 0);
	maya_w32(chip, REG_CLOCK_STAT, 0);
	chip->group_running = false;
}

/* Current ring position of an engine, in frames */
static unsigned int maya_engine_frames(struct maya44ex *chip, int n)
{
	u32 pos = maya_r32(chip, ENG_BASE(n) + ENG_POS);

	if (pos == U32_MAX)	/* device gone */
		return 0;
	return (pos & (RING_BYTES - 1)) / FRAME_BYTES;
}

/*
 * PCM
 *
 * Each of the two stereo pairs is its own PCM device. The playback/capture
 * rings are filled by the .copy/.fill_silence callbacks; a stream that starts
 * while the group is already running joins at the ring's current position.
 */

static const struct snd_pcm_hardware maya_pcm_hw = {
	.info = SNDRV_PCM_INFO_INTERLEAVED | SNDRV_PCM_INFO_BLOCK_TRANSFER,
	.formats = SNDRV_PCM_FMTBIT_S32_LE,
	.rates = SNDRV_PCM_RATE_KNOT,
	.rate_min = 32000,
	.rate_max = 48000,
	.channels_min = 2,
	.channels_max = 2,
	.buffer_bytes_max = RING_BYTES,
	.period_bytes_min = PERIOD_BYTES,
	.period_bytes_max = PERIOD_BYTES,
	.periods_min = 2,
	.periods_max = 2,
};

static struct maya_stream *maya_stream_of(struct maya44ex *chip,
					  struct snd_pcm_substream *ss)
{
	return &chip->stream[ss->pcm->device][ss->stream];
}

static int maya_engine_of(struct snd_pcm_substream *ss)
{
	int base = ss->stream == SNDRV_PCM_STREAM_PLAYBACK ? ENG_OUT12 : ENG_IN12;

	return base + ss->pcm->device;
}

static int maya_pcm_open(struct snd_pcm_substream *ss)
{
	struct maya44ex *chip = snd_pcm_substream_chip(ss);
	struct snd_pcm_runtime *runtime = ss->runtime;
	int err;

	runtime->hw = maya_pcm_hw;
	err = snd_pcm_hw_constraint_list(runtime, 0, SNDRV_PCM_HW_PARAM_RATE,
					 unverified_rates ? &maya_constraint_all :
					 &maya_constraint_verified);
	if (err < 0)
		return err;
	err = snd_pcm_hw_constraint_integer(runtime, SNDRV_PCM_HW_PARAM_PERIODS);
	if (err < 0)
		return err;

	maya_stream_of(chip, ss)->substream = ss;
	return 0;
}

static int maya_pcm_close(struct snd_pcm_substream *ss)
{
	struct maya44ex *chip = snd_pcm_substream_chip(ss);

	maya_stream_of(chip, ss)->substream = NULL;
	return 0;
}

static int maya_pcm_hw_params(struct snd_pcm_substream *ss,
			      struct snd_pcm_hw_params *hw_params)
{
	struct maya44ex *chip = snd_pcm_substream_chip(ss);
	unsigned long flags;
	int err = 0;

	spin_lock_irqsave(&chip->lock, flags);
	if (chip->nrunning && chip->rate != params_rate(hw_params))
		err = -EBUSY;	/* another stream owns the clock */
	spin_unlock_irqrestore(&chip->lock, flags);
	return err;
}

static int maya_pcm_prepare(struct snd_pcm_substream *ss)
{
	struct maya44ex *chip = snd_pcm_substream_chip(ss);
	struct maya_stream *st = maya_stream_of(chip, ss);
	unsigned long flags;

	spin_lock_irqsave(&chip->lock, flags);
	st->base = 0;
	if (ss->stream == SNDRV_PCM_STREAM_PLAYBACK)
		memset(chip->ring[maya_engine_of(ss)].area, 0, RING_BYTES);
	spin_unlock_irqrestore(&chip->lock, flags);
	return 0;
}

/* Rotate a ring so that its former frame 0 lands at frame @base */
static void maya_ring_rotate(struct maya44ex *chip, int eng, unsigned int base)
{
	u8 *ring = chip->ring[eng].area;
	unsigned int off = base * FRAME_BYTES;

	memcpy(chip->scratch, ring, RING_BYTES);
	memcpy(ring + off, chip->scratch, RING_BYTES - off);
	memcpy(ring, chip->scratch + RING_BYTES - off, off);
}

static int maya_pcm_trigger(struct snd_pcm_substream *ss, int cmd)
{
	struct maya44ex *chip = snd_pcm_substream_chip(ss);
	struct maya_stream *st = maya_stream_of(chip, ss);
	bool playback = ss->stream == SNDRV_PCM_STREAM_PLAYBACK;
	int eng = maya_engine_of(ss);
	int err = 0;

	spin_lock(&chip->lock);
	switch (cmd) {
	case SNDRV_PCM_TRIGGER_START:
		if (!chip->group_running) {
			chip->rate = ss->runtime->rate;
			err = maya_group_start(chip);
			st->base = 0;
		} else if (chip->rate != ss->runtime->rate) {
			err = -EBUSY;
		} else {
			st->base = maya_engine_frames(chip, eng) % RING_FRAMES;
			if (st->base && playback)
				maya_ring_rotate(chip, eng, st->base);
		}
		if (!err) {
			st->running = true;
			chip->nrunning++;
		}
		break;
	case SNDRV_PCM_TRIGGER_STOP:
		if (!st->running)
			break;
		st->running = false;
		chip->nrunning--;
		if (playback)
			memset(chip->ring[eng].area, 0, RING_BYTES);
		if (!chip->nrunning)
			maya_group_stop(chip);
		break;
	default:
		err = -EINVAL;
	}
	spin_unlock(&chip->lock);
	return err;
}

static snd_pcm_uframes_t maya_pcm_pointer(struct snd_pcm_substream *ss)
{
	struct maya44ex *chip = snd_pcm_substream_chip(ss);
	struct maya_stream *st = maya_stream_of(chip, ss);
	unsigned int frames = maya_engine_frames(chip, maya_engine_of(ss));

	return (frames + RING_FRAMES - st->base) % RING_FRAMES;
}

static int maya_pcm_copy(struct snd_pcm_substream *ss, int channel,
			 unsigned long pos, struct iov_iter *iter,
			 unsigned long bytes)
{
	struct maya44ex *chip = snd_pcm_substream_chip(ss);
	struct maya_stream *st = maya_stream_of(chip, ss);
	bool playback = ss->stream == SNDRV_PCM_STREAM_PLAYBACK;
	u8 *ring = chip->ring[maya_engine_of(ss)].area;
	unsigned long frame = pos / FRAME_BYTES;
	unsigned long frames = bytes / FRAME_BYTES;

	while (frames) {
		unsigned long idx = (frame + st->base) % RING_FRAMES;
		unsigned long n = min(frames, RING_FRAMES - idx);
		size_t len = n * FRAME_BYTES;
		size_t done;

		if (playback)
			done = copy_from_iter(ring + idx * FRAME_BYTES, len, iter);
		else
			done = copy_to_iter(ring + idx * FRAME_BYTES, len, iter);
		if (done != len)
			return -EFAULT;
		frame += n;
		frames -= n;
	}
	return 0;
}

static int maya_pcm_silence(struct snd_pcm_substream *ss, int channel,
			    unsigned long pos, unsigned long bytes)
{
	struct maya44ex *chip = snd_pcm_substream_chip(ss);
	struct maya_stream *st = maya_stream_of(chip, ss);
	u8 *ring = chip->ring[maya_engine_of(ss)].area;
	unsigned long frame = pos / FRAME_BYTES;
	unsigned long frames = bytes / FRAME_BYTES;

	while (frames) {
		unsigned long idx = (frame + st->base) % RING_FRAMES;
		unsigned long n = min(frames, RING_FRAMES - idx);

		memset(ring + idx * FRAME_BYTES, 0, n * FRAME_BYTES);
		frame += n;
		frames -= n;
	}
	return 0;
}

static const struct snd_pcm_ops maya_pcm_ops = {
	.open = maya_pcm_open,
	.close = maya_pcm_close,
	.hw_params = maya_pcm_hw_params,
	.prepare = maya_pcm_prepare,
	.trigger = maya_pcm_trigger,
	.pointer = maya_pcm_pointer,
	.copy = maya_pcm_copy,
	.fill_silence = maya_pcm_silence,
};

static int maya_pcm_new(struct maya44ex *chip)
{
	struct snd_pcm *pcm;
	int pair, err;

	for (pair = 0; pair < NUM_PAIRS; pair++) {
		err = snd_pcm_new(chip->card, "MAYA44eX", pair, 1, 1, &pcm);
		if (err < 0)
			return err;
		pcm->private_data = chip;
		chip->pcm[pair] = pcm;
		snprintf(pcm->name, sizeof(pcm->name), "MAYA44eX analog %s",
			 pair ? "3/4" : "1/2");
		snd_pcm_set_ops(pcm, SNDRV_PCM_STREAM_PLAYBACK, &maya_pcm_ops);
		snd_pcm_set_ops(pcm, SNDRV_PCM_STREAM_CAPTURE, &maya_pcm_ops);
	}
	return 0;
}

/*
 * Interrupt
 */

static irqreturn_t maya_interrupt(int irq, void *dev_id)
{
	struct maya44ex *chip = dev_id;
	struct snd_pcm_substream *elapsed[NUM_PAIRS * 2];
	unsigned long flags;
	u32 cause;
	int n, i, j, count = 0;

	cause = maya_r32(chip, REG_IRQ_CAUSE);
	if (!cause || cause == U32_MAX)
		return IRQ_NONE;

	spin_lock_irqsave(&chip->lock, flags);
	for (n = 0; n < NUM_ENGINES; n++) {
		if (!(cause & BIT(n)))
			continue;
		maya_r8(chip, ENG_BASE(n) + ENG_STATUS);
		if (n == IRQ_ENGINE) {
			/* re-arm, as the vendor ISR does */
			maya_w8(chip, ENG_STATE(n), 7);
		} else {
			maya_w8(chip, ENG_BASE(n) + ENG_STATUS, 0xff);
			maya_w8(chip, ENG_STATE(n), 3);
		}
	}
	for (i = 0; i < NUM_PAIRS; i++)
		for (j = 0; j < 2; j++)
			if (chip->stream[i][j].running)
				elapsed[count++] = chip->stream[i][j].substream;
	spin_unlock_irqrestore(&chip->lock, flags);

	/* all rings advance in lockstep with the interrupt engine */
	if (cause & BIT(IRQ_ENGINE))
		for (i = 0; i < count; i++)
			snd_pcm_period_elapsed(elapsed[i]);
	return IRQ_HANDLED;
}

/*
 * Mixer
 */

static const DECLARE_TLV_DB_SCALE(maya_dac_tlv, -12750, 50, 1);
static const DECLARE_TLV_DB_SCALE(maya_adc_tlv, -10350, 50, 0);

/* private_value: left register | right register << 8 | codec << 16 */
#define MAYA_VOL(l, r, codec)	((l) | ((r) << 8) | ((codec) << 16))

static int maya_vol_info(struct snd_kcontrol *kcontrol,
			 struct snd_ctl_elem_info *uinfo)
{
	uinfo->type = SNDRV_CTL_ELEM_TYPE_INTEGER;
	uinfo->count = 2;
	uinfo->value.integer.min = 0;
	uinfo->value.integer.max = 255;
	return 0;
}

static int maya_vol_get(struct snd_kcontrol *kcontrol,
			struct snd_ctl_elem_value *ucontrol)
{
	struct maya44ex *chip = snd_kcontrol_chip(kcontrol);
	unsigned long pv = kcontrol->private_value;
	int codec = (pv >> 16) & 1;

	mutex_lock(&chip->codec_lock);
	ucontrol->value.integer.value[0] = chip->codec_reg[codec][pv & 0xff] & 0xff;
	ucontrol->value.integer.value[1] = chip->codec_reg[codec][(pv >> 8) & 0xff] & 0xff;
	mutex_unlock(&chip->codec_lock);
	return 0;
}

static int maya_vol_put(struct snd_kcontrol *kcontrol,
			struct snd_ctl_elem_value *ucontrol)
{
	struct maya44ex *chip = snd_kcontrol_chip(kcontrol);
	unsigned long pv = kcontrol->private_value;
	int codec = (pv >> 16) & 1;
	u8 reg[2] = { pv & 0xff, (pv >> 8) & 0xff };
	int i, changed = 0, err = 0;

	mutex_lock(&chip->codec_lock);
	for (i = 0; i < 2; i++) {
		u16 val = ucontrol->value.integer.value[i] & 0xff;

		if ((chip->codec_reg[codec][reg[i]] & 0xff) == val)
			continue;
		err = maya_codec_write(chip, codec, reg[i], val | WM_UPDATE);
		if (err)
			break;
		changed = 1;
	}
	mutex_unlock(&chip->codec_lock);
	return err ? err : changed;
}

#define maya_sw_info	snd_ctl_boolean_mono_info

static int maya_sw_get(struct snd_kcontrol *kcontrol,
		       struct snd_ctl_elem_value *ucontrol)
{
	struct maya44ex *chip = snd_kcontrol_chip(kcontrol);
	int codec = kcontrol->private_value & 1;

	mutex_lock(&chip->codec_lock);
	/* the codec bit is "mute"; the control is "playback switch" */
	ucontrol->value.integer.value[0] = !(chip->codec_reg[codec][WM_DAC_MUTE] & 1);
	mutex_unlock(&chip->codec_lock);
	return 0;
}

static int maya_sw_put(struct snd_kcontrol *kcontrol,
		       struct snd_ctl_elem_value *ucontrol)
{
	struct maya44ex *chip = snd_kcontrol_chip(kcontrol);
	int codec = kcontrol->private_value & 1;
	u16 val = ucontrol->value.integer.value[0] ? 0 : 1;
	int err = 0, changed = 0;

	mutex_lock(&chip->codec_lock);
	if ((chip->codec_reg[codec][WM_DAC_MUTE] & 1) != val) {
		err = maya_codec_write(chip, codec, WM_DAC_MUTE, val);
		changed = !err;
	}
	mutex_unlock(&chip->codec_lock);
	return err ? err : changed;
}

static int maya_mixer_new(struct maya44ex *chip)
{
	int codec, err;

	strscpy(chip->card->mixername, "MAYA44eX WM8776");

	for (codec = 0; codec < 2; codec++) {
		struct snd_kcontrol_new controls[] = {
			{
				.iface = SNDRV_CTL_ELEM_IFACE_MIXER,
				.name = "PCM Playback Volume",
				.access = SNDRV_CTL_ELEM_ACCESS_READWRITE |
					  SNDRV_CTL_ELEM_ACCESS_TLV_READ,
				.info = maya_vol_info,
				.get = maya_vol_get,
				.put = maya_vol_put,
				.tlv.p = maya_dac_tlv,
				.private_value = MAYA_VOL(WM_DAC_ATTEN_L,
							  WM_DAC_ATTEN_R, codec),
			},
			{
				.iface = SNDRV_CTL_ELEM_IFACE_MIXER,
				.name = "PCM Playback Switch",
				.info = maya_sw_info,
				.get = maya_sw_get,
				.put = maya_sw_put,
				.private_value = codec,
			},
			{
				.iface = SNDRV_CTL_ELEM_IFACE_MIXER,
				.name = "Capture Volume",
				.access = SNDRV_CTL_ELEM_ACCESS_READWRITE |
					  SNDRV_CTL_ELEM_ACCESS_TLV_READ,
				.info = maya_vol_info,
				.get = maya_vol_get,
				.put = maya_vol_put,
				.tlv.p = maya_adc_tlv,
				.private_value = MAYA_VOL(WM_ADC_GAIN_L,
							  WM_ADC_GAIN_R, codec),
			},
		};
		int i;

		for (i = 0; i < ARRAY_SIZE(controls); i++) {
			controls[i].index = codec;
			err = snd_ctl_add(chip->card, snd_ctl_new1(&controls[i], chip));
			if (err < 0)
				return err;
		}
	}
	return 0;
}

/*
 * Chip initialisation
 */

/* Replays the vendor driver's start-up. Also used on resume. */
static int maya_chip_init(struct maya44ex *chip)
{
	int i, codec, err;

	maya_w32(chip, REG_RESET, 0);
	maya_w32(chip, REG_RESET, 0x80000000);
	maya_w32(chip, REG_CLK_BB_CTRL, 0x38000);
	for (i = 0; i < 40; i++) {
		maya_w32(chip, REG_CLK_BB, (i & 1) ? 0x68a6d995 : 0x68e6d995);
		udelay(10);
	}
	msleep(20);

	maya_w8(chip, REG_GPIO_A, 0);
	maya_w8(chip, REG_GPIO_B, 0);
	maya_w32(chip, REG_CLOCK_SEL, 0x80000000);
	maya_w32(chip, REG_CLOCK_STAT, 0x80000000);
	for (i = 0; i < NUM_ENGINES; i++)
		maya_w16(chip, REG_RATE_CLK(i), 0x4031);

	for (i = 0; i < 4; i++) {
		u8 b;

		if (maya_eeprom_read(chip, i, &b)) {
			dev_warn(chip->card->dev, "EEPROM read timed out\n");
			break;
		}
	}

	maya_w8(chip, REG_ROUTE_A, 0xff);
	maya_w8(chip, REG_ROUTE, 0xaa);

	for (codec = 0; codec < 2; codec++) {
		for (i = 0; i < ARRAY_SIZE(maya_codec_init); i++) {
			err = maya_codec_write(chip, codec, maya_codec_init[i][0],
					       maya_codec_init[i][1]);
			if (err)
				return err;
		}
	}
	maya_w8(chip, REG_ROUTE, 0x2a);
	return 0;
}

static void maya_chip_stop(struct maya44ex *chip)
{
	unsigned long flags;

	spin_lock_irqsave(&chip->lock, flags);
	maya_group_stop(chip);
	spin_unlock_irqrestore(&chip->lock, flags);
	maya_w32(chip, REG_DMA_GLOBAL, 0);
	maya_w32(chip, REG_DMA_GLOBAL2, 0);
}

/*
 * Chip create / free
 */

static void maya_chip_free(struct snd_card *card)
{
	struct maya44ex *chip = card->private_data;
	int n;

	if (chip->iobase)
		maya_chip_stop(chip);
	if (chip->irq >= 0)
		free_irq(chip->irq, chip);
	for (n = 0; n < NUM_ENGINES; n++)
		if (chip->ring[n].area)
			snd_dma_free_pages(&chip->ring[n]);
	if (chip->tables.area)
		snd_dma_free_pages(&chip->tables);
	kfree(chip->scratch);
	if (chip->iobase)
		pci_iounmap(chip->pci, chip->iobase);
	pci_release_regions(chip->pci);
	pci_disable_device(chip->pci);
}

static int maya_chip_create(struct snd_card *card, struct pci_dev *pci)
{
	struct maya44ex *chip = card->private_data;
	int n, err;

	chip->card = card;
	chip->pci = pci;
	chip->irq = -1;
	chip->rate = 44100;
	spin_lock_init(&chip->lock);
	mutex_init(&chip->codec_lock);
	card->private_free = maya_chip_free;

	err = pci_enable_device(pci);
	if (err < 0)
		return err;
	err = dma_set_mask_and_coherent(&pci->dev, DMA_BIT_MASK(32));
	if (err < 0) {
		pci_disable_device(pci);
		return err;
	}
	err = pci_request_regions(pci, "MAYA44eX");
	if (err < 0) {
		pci_disable_device(pci);
		return err;
	}

	/* from here on maya_chip_free() releases everything */
	chip->iobase = pci_iomap(pci, 0, 0);
	if (!chip->iobase)
		return -ENOMEM;
	pci_set_master(pci);

	/* all-ones means memory decode is off or the device is gone */
	if (maya_r32(chip, REG_IRQ_CAUSE) == U32_MAX) {
		dev_err(&pci->dev, "BAR 0 reads all-ones, refusing to bind\n");
		return -ENODEV;
	}

	for (n = 0; n < NUM_ENGINES; n++) {
		err = snd_dma_alloc_pages(SNDRV_DMA_TYPE_DEV, &pci->dev,
					  RING_BYTES, &chip->ring[n]);
		if (err < 0)
			return err;
	}
	err = snd_dma_alloc_pages(SNDRV_DMA_TYPE_DEV, &pci->dev,
				  TABLE_AREA_BYTES, &chip->tables);
	if (err < 0)
		return err;
	chip->scratch = kmalloc(RING_BYTES, GFP_KERNEL);
	if (!chip->scratch)
		return -ENOMEM;

	err = maya_chip_init(chip);
	if (err < 0)
		return err;

	if (request_irq(pci->irq, maya_interrupt, IRQF_SHARED,
			KBUILD_MODNAME, chip)) {
		dev_err(&pci->dev, "unable to grab IRQ %d\n", pci->irq);
		return -EBUSY;
	}
	chip->irq = pci->irq;
	card->sync_irq = chip->irq;
	return 0;
}

/*
 * Power management
 */

static int __maybe_unused maya_suspend(struct device *dev)
{
	struct snd_card *card = dev_get_drvdata(dev);
	struct maya44ex *chip = card->private_data;
	int pair, dir;

	snd_power_change_state(card, SNDRV_CTL_POWER_D3hot);
	for (pair = 0; pair < NUM_PAIRS; pair++)
		snd_pcm_suspend_all(chip->pcm[pair]);
	maya_chip_stop(chip);

	/* suspended streams are re-prepared and restarted by user space */
	for (pair = 0; pair < NUM_PAIRS; pair++)
		for (dir = 0; dir < 2; dir++)
			chip->stream[pair][dir].running = false;
	chip->nrunning = 0;
	return 0;
}

static int __maybe_unused maya_resume(struct device *dev)
{
	struct snd_card *card = dev_get_drvdata(dev);
	struct maya44ex *chip = card->private_data;
	u16 saved[2][WM_NUM_REGS];
	int codec, reg, err;

	memcpy(saved, chip->codec_reg, sizeof(saved));
	pci_set_master(chip->pci);
	err = maya_chip_init(chip);
	if (err < 0) {
		dev_err(dev, "resume failed: %d\n", err);
		snd_card_disconnect(card);
		return err;
	}

	/* restore the user's mixer settings over the power-on defaults */
	for (codec = 0; codec < 2; codec++)
		for (reg = WM_DAC_ATTEN_L; reg <= WM_ADC_GAIN_R; reg++)
			if (reg == WM_DAC_ATTEN_L || reg == WM_DAC_ATTEN_R ||
			    reg == WM_DAC_MUTE || reg == WM_ADC_GAIN_L ||
			    reg == WM_ADC_GAIN_R)
				maya_codec_write(chip, codec, reg,
						 saved[codec][reg] | WM_UPDATE);

	snd_power_change_state(card, SNDRV_CTL_POWER_D0);
	return 0;
}

static SIMPLE_DEV_PM_OPS(maya_pm, maya_suspend, maya_resume);

/*
 * PCI probe / remove
 */

static int maya_probe(struct pci_dev *pci, const struct pci_device_id *pci_id)
{
	static int dev;
	struct snd_card *card;
	int err;

	if (dev >= SNDRV_CARDS)
		return -ENODEV;
	if (!enable[dev]) {
		dev++;
		return -ENOENT;
	}

	err = snd_card_new(&pci->dev, index[dev], id[dev], THIS_MODULE,
			   sizeof(struct maya44ex), &card);
	if (err < 0)
		return err;

	strscpy(card->driver, "MAYA44eX");
	strscpy(card->shortname, "ESI MAYA44 eX");
	snprintf(card->longname, sizeof(card->longname),
		 "%s (ES7010L) at 0x%lx, irq %i", card->shortname,
		 (unsigned long)pci_resource_start(pci, 0), pci->irq);

	err = maya_chip_create(card, pci);
	if (err < 0)
		goto error;
	err = maya_pcm_new(card->private_data);
	if (err < 0)
		goto error;
	err = maya_mixer_new(card->private_data);
	if (err < 0)
		goto error;
	err = snd_card_register(card);
	if (err < 0)
		goto error;

	pci_set_drvdata(pci, card);
	dev++;
	return 0;

error:
	snd_card_free(card);
	return err;
}

static void maya_remove(struct pci_dev *pci)
{
	snd_card_free(pci_get_drvdata(pci));
}

static const struct pci_device_id maya_ids[] = {
	{ PCI_DEVICE(PCI_VENDOR_ID_ESI, PCI_DEVICE_ID_MAYA44EX) },
	{ }
};
MODULE_DEVICE_TABLE(pci, maya_ids);

static struct pci_driver maya_driver = {
	.name = KBUILD_MODNAME,
	.id_table = maya_ids,
	.probe = maya_probe,
	.remove = maya_remove,
	.driver = {
		.pm = &maya_pm,
	},
};

module_pci_driver(maya_driver);
