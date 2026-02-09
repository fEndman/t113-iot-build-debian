// SPDX-License-Identifier: GPL-2.0+
/*
 * DRM driver for display panels connected to a Sitronix ST7715R or ST7735R
 * display controller in SPI mode.
 *
 * Copyright 2017 David Lechner <david@lechnology.com>
 * Copyright (C) 2019 Glider bvba
 */

#include <linux/backlight.h>
#include <linux/delay.h>
#include <linux/dma-buf.h>
#include <linux/gpio/consumer.h>
#include <linux/module.h>
#include <linux/property.h>
#include <linux/spi/spi.h>

#include <video/mipi_display.h>
#include <drm/drm_mipi_dbi.h>

#include <drm/drm_connector.h>
#include <drm/drm_damage_helper.h>
#include <drm/drm_drv.h>
#include <drm/drm_file.h>
#include <drm/drm_format_helper.h>
#include <drm/drm_fourcc.h>
#include <drm/drm_framebuffer.h>
#include <drm/drm_gem.h>
#include <drm/drm_gem_framebuffer_helper.h>
#include <drm/drm_modes.h>
#include <drm/drm_probe_helper.h>
#include <drm/drm_rect.h>
#include <drm/drm_atomic_helper.h>
#include <drm/drm_fb_helper.h>
#include <drm/drm_gem_atomic_helper.h>
#include <drm/drm_gem_dma_helper.h>
#include <drm/drm_managed.h>

#include "../../../spi/spi-sunxi.h"

#define ST7735R_FRMCTR1		0xb1
#define ST7735R_FRMCTR2		0xb2
#define ST7735R_FRMCTR3		0xb3
#define ST7735R_INVCTR		0xb4
#define ST7735R_PWCTR1		0xc0
#define ST7735R_PWCTR2		0xc1
#define ST7735R_PWCTR3		0xc2
#define ST7735R_PWCTR4		0xc3
#define ST7735R_PWCTR5		0xc4
#define ST7735R_VMCTR1		0xc5
#define ST7735R_GAMCTRP1	0xe0
#define ST7735R_GAMCTRN1	0xe1

#define ST7735R_MY	BIT(7)
#define ST7735R_MX	BIT(6)
#define ST7735R_MV	BIT(5)
#define ST7735R_BGR	BIT(3)

struct sunxi_dbi_dev {
	struct drm_device drm;
	struct drm_simple_display_pipe pipe;
	struct drm_connector connector;
	struct drm_framebuffer *fb;
    u16 *vram;
};

struct st7735r_cfg {
	const struct drm_display_mode mode;
	unsigned int left_offset;
	unsigned int top_offset;
	unsigned int write_only:1;
	unsigned int bgr:1;
};

struct st7735r_priv {
	struct sunxi_dbi_dev dbi_dev;
    struct spi_device *spi;
    struct spi_dbi_config dbi_cfg;
    const struct st7735r_cfg *cfg;
	struct gpio_desc *reset;
	u32 rotation;
    struct mutex cmdlock;
    struct backlight_device *backlight;
};

int sunxi_dbi_transfer(struct st7735r_priv *priv, const void *data, size_t len)
{
	u8 *buf;
	int ret;
	// size_t max_chunk = spi_max_transfer_size(priv->spi);
	struct spi_transfer tr = {
		.bits_per_word = 8,
		.speed_hz = 500000000,
	};
	struct spi_message m;
	// size_t chunk;

	mutex_lock(&priv->cmdlock);

	/* SPI requires dma-safe buffers */
	buf = kmemdup(data, len, GFP_KERNEL);
	if (!buf){
        printk("dbi: transfer mem error");
        goto err_dbi_transfer;
    }

	/*  see drivers/gpu/drm/drm_mipi_dbi.c : line 1203 */
	// max_chunk = ALIGN_DOWN(max_chunk, 2);

	spi_message_init_with_transfers(&m, &tr, 1);


		tr.tx_buf = buf;
		tr.len = len;

		ret = spi_sync(priv->spi, &m);
		if (ret){
            printk("dbi: transfer error");
			goto err_dbi_transfer_buf;
        }
	// while (len) {
	// 	chunk = min(len, max_chunk);

	// 	tr.tx_buf = buf;
	// 	tr.len = chunk;
	// 	buf += chunk;
	// 	len -= chunk;

	// 	ret = spi_sync(priv->spi, &m);
	// 	if (ret){
    //         printk("dbi: transfer error");
	// 		goto err_dbi_transfer_buf;
    //     }
	// }

err_dbi_transfer_buf:
	kfree(buf);
err_dbi_transfer:
	mutex_unlock(&priv->cmdlock);

	return ret;
}

#define sunxi_dbi_command(priv, cmd, seq...) \
({ \
	const u8 c[] = { cmd }; \
	const u8 d[] = { seq }; \
    DBI_DCX_COMMAND(priv->dbi_cfg.dbi_mode); \
    spi_set_dbi_config(priv->spi, &priv->dbi_cfg); \
	sunxi_dbi_transfer(priv, c, 1); \
    DBI_DCX_DATA(priv->dbi_cfg.dbi_mode); \
    spi_set_dbi_config(priv->spi, &priv->dbi_cfg); \
	sunxi_dbi_transfer(priv, d, ARRAY_SIZE(d)); \
})

static void sunxi_dbi_set_frame(struct drm_framebuffer *fb)
{
	struct st7735r_priv *priv = container_of(fb->dev, 
                                    struct st7735r_priv, dbi_dev.drm);


    priv->dbi_dev.fb = fb;

}

void sunxi_dbi_vsync_handle(unsigned long data)
{
    struct spi_device *spi = (struct spi_device*)data;
    struct drm_device *drm = (struct drm_device*)spi_get_drvdata(spi);
	struct st7735r_priv *priv = container_of(drm, 
                                    struct st7735r_priv, dbi_dev.drm);
	int idx;
    static u32 count = 0;


	struct iosys_map map[DRM_FORMAT_MAX_PLANES];
	struct iosys_map vdata[DRM_FORMAT_MAX_PLANES];
	struct iosys_map vram_map = IOSYS_MAP_INIT_VADDR(priv->dbi_dev.vram);
	struct drm_rect rect_full = DRM_RECT_INIT(0, 0, 
                    priv->dbi_cfg.dbi_video_h, priv->dbi_cfg.dbi_video_v); 
    struct drm_framebuffer *fb = priv->dbi_dev.fb;

        // printk("dbi: set frame");

	if (!drm_dev_enter(fb->dev, &idx))
		return;

	if (drm_gem_fb_begin_cpu_access(fb, DMA_FROM_DEVICE))
		goto err_dbi_vsync;

	if (drm_gem_fb_vmap(fb, map, vdata))
		goto err_dbi_vsync_gem;
    drm_fb_memcpy(&vram_map, NULL, vdata, fb, &rect_full);




        count++;
        if(count % 30 == 0)
            printk("dbi: vsync");

	// if (!drm_dev_enter(drm, &idx))
	// 	return;

    DBI_WRITE(priv->dbi_cfg.dbi_mode);
    DBI_TR_VIDEO(priv->dbi_cfg.dbi_mode);
    spi_set_dbi_config(priv->spi, &priv->dbi_cfg);
    sunxi_dbi_transfer(priv, priv->dbi_dev.vram, 
        priv->dbi_cfg.dbi_video_v * priv->dbi_cfg.dbi_video_h * sizeof(u16));

	// drm_dev_exit(idx);



    drm_gem_fb_vunmap(fb, map);
err_dbi_vsync_gem:
	drm_gem_fb_end_cpu_access(fb, DMA_FROM_DEVICE);
err_dbi_vsync:
	drm_dev_exit(idx);
}

static void st7735r_pipe_enable(struct drm_simple_display_pipe *pipe,
				struct drm_crtc_state *crtc_state,
				struct drm_plane_state *plane_state)
{
	struct st7735r_priv *priv = container_of(pipe->crtc.dev, 
                                    struct st7735r_priv, dbi_dev.drm);
	int idx;
	u8 addr_mode;
	u16 xs = priv->cfg->left_offset;
	u16 xe = priv->dbi_cfg.dbi_video_h + priv->cfg->left_offset;
	u16 ys = priv->cfg->top_offset;
	u16 ye = priv->dbi_cfg.dbi_video_v + priv->cfg->top_offset;


        // printk("dbi: pipe enalbe");


	if (!drm_dev_enter(pipe->crtc.dev, &idx))
		return;

    /* hw reset */
	gpiod_set_value_cansleep(priv->reset, 0);
	msleep(5);
	gpiod_set_value_cansleep(priv->reset, 1);
	msleep(120);

    DBI_WRITE(priv->dbi_cfg.dbi_mode);
    DBI_TR_COMMAND(priv->dbi_cfg.dbi_mode);
    spi_set_dbi_config(priv->spi, &priv->dbi_cfg);

    /* soft reset */    
	sunxi_dbi_command(priv, MIPI_DCS_SOFT_RESET);
    msleep(5);

	sunxi_dbi_command(priv, MIPI_DCS_EXIT_SLEEP_MODE);
	msleep(500);

	sunxi_dbi_command(priv, ST7735R_FRMCTR1, 0x01, 0x2c, 0x2d);
	sunxi_dbi_command(priv, ST7735R_FRMCTR2, 0x01, 0x2c, 0x2d);
	sunxi_dbi_command(priv, ST7735R_FRMCTR3, 0x01, 0x2c, 0x2d, 0x01, 0x2c,
			 0x2d);
	sunxi_dbi_command(priv, ST7735R_INVCTR, 0x07);
	sunxi_dbi_command(priv, ST7735R_PWCTR1, 0xa2, 0x02, 0x84);
	sunxi_dbi_command(priv, ST7735R_PWCTR2, 0xc5);
	sunxi_dbi_command(priv, ST7735R_PWCTR3, 0x0a, 0x00);
	sunxi_dbi_command(priv, ST7735R_PWCTR4, 0x8a, 0x2a);
	sunxi_dbi_command(priv, ST7735R_PWCTR5, 0x8a, 0xee);
	sunxi_dbi_command(priv, ST7735R_VMCTR1, 0x0e);
	sunxi_dbi_command(priv, MIPI_DCS_EXIT_INVERT_MODE);
	switch (priv->rotation) {
	default:
		addr_mode = ST7735R_MX | ST7735R_MY;
		break;
	case 90:
		addr_mode = ST7735R_MX | ST7735R_MV;
		break;
	case 180:
		addr_mode = 0;
		break;
	case 270:
		addr_mode = ST7735R_MY | ST7735R_MV;
		break;
	}

	if (priv->cfg->bgr)
		addr_mode |= ST7735R_BGR;

	sunxi_dbi_command(priv, MIPI_DCS_SET_ADDRESS_MODE, addr_mode);
	sunxi_dbi_command(priv, MIPI_DCS_SET_PIXEL_FORMAT,
			 MIPI_DCS_PIXEL_FMT_16BIT);
	sunxi_dbi_command(priv, ST7735R_GAMCTRP1, 0x02, 0x1c, 0x07, 0x12, 0x37,
			 0x32, 0x29, 0x2d, 0x29, 0x25, 0x2b, 0x39, 0x00, 0x01,
			 0x03, 0x10);
	sunxi_dbi_command(priv, ST7735R_GAMCTRN1, 0x03, 0x1d, 0x07, 0x06, 0x2e,
			 0x2c, 0x29, 0x2d, 0x2e, 0x2e, 0x37, 0x3f, 0x00, 0x00,
			 0x02, 0x10);
	sunxi_dbi_command(priv, MIPI_DCS_SET_DISPLAY_ON);

	msleep(100);

	sunxi_dbi_command(priv, MIPI_DCS_ENTER_NORMAL_MODE);

	msleep(20);

	sunxi_dbi_command(priv, MIPI_DCS_SET_COLUMN_ADDRESS, 
                (xs >> 8) & 0xff, xs & 0xff, (xe >> 8) & 0xff, xe & 0xff);
	sunxi_dbi_command(priv, MIPI_DCS_SET_PAGE_ADDRESS, 
                (ys >> 8) & 0xff, ys & 0xff, (ye >> 8) & 0xff, ye & 0xff);
	sunxi_dbi_command(priv, MIPI_DCS_WRITE_MEMORY_START);

    sunxi_dbi_set_frame(plane_state->fb);
    sunxi_dbi_vsync_handle((unsigned long)priv->spi);
	
    backlight_enable(priv->backlight);

	drm_dev_exit(idx);
}

void st7735r_pipe_disable(struct drm_simple_display_pipe *pipe)
{
	struct st7735r_priv *priv = container_of(pipe->crtc.dev, 
                                    struct st7735r_priv, dbi_dev.drm);


        // printk("dbi: pipe disable");

    DBI_WRITE(priv->dbi_cfg.dbi_mode);
    DBI_TR_COMMAND(priv->dbi_cfg.dbi_mode);
    spi_set_dbi_config(priv->spi, &priv->dbi_cfg);

	sunxi_dbi_command(priv, MIPI_DCS_SET_DISPLAY_OFF);
	sunxi_dbi_command(priv, MIPI_DCS_ENTER_SLEEP_MODE);

	backlight_disable(priv->backlight);
}

void sunxi_dbi_pipe_update(struct drm_simple_display_pipe *pipe,
			  struct drm_plane_state *old_state)
{
	if (pipe->crtc.state->active)
        sunxi_dbi_set_frame(pipe->plane.state->fb);
}

enum drm_mode_status sunxi_dbi_pipe_mode_valid(struct drm_simple_display_pipe *pipe,
					      const struct drm_display_mode *mode)
{
	struct st7735r_priv *priv = container_of(pipe->crtc.dev, 
                                    struct st7735r_priv, dbi_dev.drm);

        // printk("dbi: mod valid");

	return drm_crtc_helper_mode_valid_fixed(&pipe->crtc, mode, &priv->cfg->mode);
}

static const struct drm_simple_display_pipe_funcs st7735r_pipe_funcs = {
	.mode_valid	= sunxi_dbi_pipe_mode_valid,
	.enable		= st7735r_pipe_enable,
	.disable	= st7735r_pipe_disable,
	.update		= sunxi_dbi_pipe_update,
};

static const struct st7735r_cfg yyh_tft18019_cfg = {
	.mode		= { DRM_SIMPLE_MODE(128, 160, 28, 35) },
    .write_only = true,
	.left_offset = 1,
	.top_offset = 2,
};

DEFINE_DRM_GEM_DMA_FOPS(st7735r_fops);

static const struct drm_driver st7735r_driver = {
	.driver_features	= DRIVER_GEM | DRIVER_MODESET | DRIVER_ATOMIC,
	.fops			= &st7735r_fops,
	DRM_GEM_DMA_DRIVER_OPS_VMAP,
	// .debugfs_init		= mipi_dbi_debugfs_init,
	.name			= "st7735r",
	.desc			= "Sitronix ST7735R",
	.date			= "20230119",
	.major			= 1,
	.minor			= 0,
};

static const struct of_device_id st7735r_of_match[] = {
	{ .compatible = "yyh,tft18019", .data = &yyh_tft18019_cfg },
	{ },
};
MODULE_DEVICE_TABLE(of, st7735r_of_match);

static const struct spi_device_id st7735r_id[] = {
	{ "tft18019", (uintptr_t)&yyh_tft18019_cfg },
	{ },
};
MODULE_DEVICE_TABLE(spi, st7735r_id);

static int sunxi_dbi_connector_get_modes(struct drm_connector *connector)
{
	struct st7735r_priv *priv = container_of(connector->dev, 
                                    struct st7735r_priv, dbi_dev.drm);

	return drm_connector_helper_get_modes_fixed(connector, &priv->cfg->mode);
}

static const struct drm_connector_helper_funcs sunxi_dbi_connector_hfuncs = {
	.get_modes = sunxi_dbi_connector_get_modes,
};

static const struct drm_connector_funcs sunxi_dbi_connector_funcs = {
	.reset = drm_atomic_helper_connector_reset,
	.fill_modes = drm_helper_probe_single_connector_modes,
	.destroy = drm_connector_cleanup,
	.atomic_duplicate_state = drm_atomic_helper_connector_duplicate_state,
	.atomic_destroy_state = drm_atomic_helper_connector_destroy_state,
};

static const uint32_t sunxi_dbi_formats[] = {
	DRM_FORMAT_RGB565,
	DRM_FORMAT_XRGB8888,
};

static const uint64_t drm_formats[] = {
    DRM_FORMAT_MOD_LINEAR,
    DRM_FORMAT_MOD_INVALID
};

static const struct drm_mode_config_funcs sunxi_dbi_mode_config_funcs = {
	.fb_create = drm_gem_fb_create,
	.atomic_check = drm_atomic_helper_check,
	.atomic_commit = drm_atomic_helper_commit,
};

static int st7735r_probe(struct spi_device *spi)
{
	struct device *dev = &spi->dev;
	const struct st7735r_cfg *cfg;
	struct sunxi_dbi_dev *dbi_dev;
	struct st7735r_priv *priv;
	struct drm_device *drm;
	// struct drm_display_mode *drm_mode;
	// struct mipi_dbi *dbi;
	// struct gpio_desc *dc;
    u32 fps;
	int ret;

	cfg = device_get_match_data(&spi->dev);
	if (!cfg)
		cfg = (void *)spi_get_device_id(spi)->driver_data;

	priv = devm_drm_dev_alloc(dev, &st7735r_driver,
				  struct st7735r_priv, dbi_dev.drm);
	if (IS_ERR(priv))
		return PTR_ERR(priv);
    
	dbi_dev = &priv->dbi_dev;
	priv->cfg = cfg;
	priv->spi = spi;

    mutex_init(&priv->cmdlock);

    dbi_dev->vram = devm_kmalloc(dev, 
                        cfg->mode.vdisplay * cfg->mode.hdisplay * sizeof(u32), 
                        GFP_KERNEL);
	if (!dbi_dev->vram)
		return -ENOMEM;

	// dbi = &dbi_dev.dbi;
	drm = &priv->dbi_dev.drm;

	priv->reset = devm_gpiod_get(dev, "reset", GPIOD_OUT_HIGH);
	if (IS_ERR(priv->reset))
		return dev_err_probe(dev, PTR_ERR(priv->reset), "Failed to get GPIO 'reset'\n");

	// dc = devm_gpiod_get(dev, "dc", GPIOD_OUT_LOW);
	// if (IS_ERR(dc))
	// 	return dev_err_probe(dev, PTR_ERR(dc), "Failed to get GPIO 'dc'\n");

	priv->backlight = devm_of_find_backlight(dev);
	if (IS_ERR(priv->backlight))
		return PTR_ERR(priv->backlight);

    if(device_property_read_u32(dev, "rotation", &priv->rotation))
        priv->rotation = 0;
    priv->rotation %= 360;
    if(priv->rotation % 90 != 0)
		return dev_err_probe(dev, -EINVAL, "Illegal rotation argument\n");

    if(device_property_read_u32(dev, "fps", &fps))
        fps = 30;

    priv->dbi_cfg.dbi_src_sequence = DBI_SRC_RGB;
    priv->dbi_cfg.dbi_out_sequence = DBI_OUT_RGB;
    priv->dbi_cfg.dbi_format = DBI_RGB565;
    priv->dbi_cfg.dbi_interface = L4I1;
    priv->dbi_cfg.dbi_video_v = cfg->mode.vdisplay;
    priv->dbi_cfg.dbi_video_h = cfg->mode.hdisplay;
    priv->dbi_cfg.dbi_fps = fps;
    priv->dbi_cfg.dbi_vsync_handle = sunxi_dbi_vsync_handle;
    if (priv->rotation == 90 || priv->rotation == 270)
		swap(priv->dbi_cfg.dbi_video_v, priv->dbi_cfg.dbi_video_h);
    DBI_WRITE(priv->dbi_cfg.dbi_mode);
    DBI_MSB_FIRST(priv->dbi_cfg.dbi_mode);
    spi_set_dbi_config(spi, &priv->dbi_cfg);


	// ret = mipi_dbi_spi_init(spi, dbi, dc);
	// if (ret)
	// 	return ret;

	// if (cfg->write_only)
	// 	dbi->read_commands = NULL;

	// dbi_dev.left_offset = cfg->left_offset;
	// dbi_dev.top_offset = cfg->top_offset;

	// ret = mipi_dbi_dev_init(dbi_dev, &st7735r_pipe_funcs, &cfg->mode,
	// 			rotation);
	// if (ret)
	// 	return ret;


    ret = dma_coerce_mask_and_coherent(dev, DMA_BIT_MASK(32));
	if (ret){
        printk("drm: dma_coerce_mask_and_coherent failed");
		return ret;
    }

	ret = drmm_mode_config_init(drm);
	if (ret){
        printk("drm: drmm_mode_config_init failed");
		return ret;
    }

    // drm_mode = drm_mode_duplicate(drm, &cfg->mode);
    // drm_mode_probed_add(&dbi_dev->connector, drm_mode);
	drm_connector_helper_add(&dbi_dev->connector, &sunxi_dbi_connector_hfuncs);
	ret = drm_connector_init(drm, &dbi_dev->connector, &sunxi_dbi_connector_funcs,
                        DRM_MODE_CONNECTOR_SPI);
	if (ret){
        printk("drm: drm_connector_init failed");
		return ret;
    }

	ret = drm_simple_display_pipe_init(drm, &dbi_dev->pipe, &st7735r_pipe_funcs, 
                        sunxi_dbi_formats, ARRAY_SIZE(sunxi_dbi_formats),
					    drm_formats, &dbi_dev->connector);
	if (ret){
        printk("drm: drm_simple_display_pipe_init failed");
		return ret;
    }

	// drm_plane_enable_fb_damage_clips(&dbi_dev->pipe.plane);

    drm->mode_config.preferred_depth = 16;
	drm->mode_config.funcs = &sunxi_dbi_mode_config_funcs;
	drm->mode_config.min_width = cfg->mode.hdisplay;
	drm->mode_config.max_width = cfg->mode.hdisplay;
	drm->mode_config.min_height = cfg->mode.vdisplay;
	drm->mode_config.max_height = cfg->mode.vdisplay;
    if (priv->rotation == 90 || priv->rotation == 270) {
		swap(drm->mode_config.min_width, drm->mode_config.min_height);
		swap(drm->mode_config.max_width, drm->mode_config.max_height);
    }




	drm_mode_config_reset(drm);

	ret = drm_dev_register(drm, 0);
	if (ret){
        printk("drm: drm_dev_register failed");
		return ret;
    }

	spi_set_drvdata(spi, drm);

	drm_fbdev_generic_setup(drm, 0);



        // printk("drm: enabled");


	return 0;
}

static void st7735r_remove(struct spi_device *spi)
{
	struct drm_device *drm = spi_get_drvdata(spi);

	drm_dev_unplug(drm);
	drm_atomic_helper_shutdown(drm);
}

static void st7735r_shutdown(struct spi_device *spi)
{
	drm_atomic_helper_shutdown(spi_get_drvdata(spi));
}

static struct spi_driver st7735r_spi_driver = {
	.driver = {
		.name = "st7735r",
		.of_match_table = st7735r_of_match,
	},
	.id_table = st7735r_id,
	.probe = st7735r_probe,
	.remove = st7735r_remove,
	.shutdown = st7735r_shutdown,
};
module_spi_driver(st7735r_spi_driver);

MODULE_DESCRIPTION("Sitronix ST7735R DRM driver");
MODULE_AUTHOR("David Lechner <david@lechnology.com>");
MODULE_LICENSE("GPL");
