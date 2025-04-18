// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2018 Texas Instruments Incorporated - https://www.ti.com/
 * Author: Tomi Valkeinen <tomi.valkeinen@ti.com>
 */

#include <linux/platform_device.h>

#include <drm/drm_drv.h>
#include <drm/drm_print.h>

#include "tidss_crtc.h"
#include "tidss_dispc.h"
#include "tidss_drv.h"
#include "tidss_irq.h"
#include "tidss_plane.h"

/* call with wait_lock and dispc runtime held */
static void tidss_irq_update(struct tidss_device *tidss)
{
	assert_spin_locked(&tidss->wait_lock);

	dispc_set_irqenable(tidss->dispc, tidss->irq_mask);
}

void tidss_irq_enable_vblank(struct drm_crtc *crtc)
{
	struct drm_device *ddev = crtc->dev;
	struct tidss_device *tidss = to_tidss(ddev);
	struct tidss_crtc *tcrtc = to_tidss_crtc(crtc);
	u32 hw_videoport = tcrtc->hw_videoport;
	unsigned long flags;

	spin_lock_irqsave(&tidss->wait_lock, flags);
	tidss->irq_mask |= DSS_IRQ_VP_VSYNC_EVEN(hw_videoport) |
			   DSS_IRQ_VP_VSYNC_ODD(hw_videoport);
	tidss_irq_update(tidss);
	spin_unlock_irqrestore(&tidss->wait_lock, flags);
}

void tidss_irq_disable_vblank(struct drm_crtc *crtc)
{
	struct drm_device *ddev = crtc->dev;
	struct tidss_device *tidss = to_tidss(ddev);
	struct tidss_crtc *tcrtc = to_tidss_crtc(crtc);
	u32 hw_videoport = tcrtc->hw_videoport;
	unsigned long flags;

	spin_lock_irqsave(&tidss->wait_lock, flags);
	tidss->irq_mask &= ~(DSS_IRQ_VP_VSYNC_EVEN(hw_videoport) |
			     DSS_IRQ_VP_VSYNC_ODD(hw_videoport));
	tidss_irq_update(tidss);
	spin_unlock_irqrestore(&tidss->wait_lock, flags);
}

static irqreturn_t tidss_irq_handler(int irq, void *arg)
{
	struct drm_device *ddev = (struct drm_device *)arg;
	struct tidss_device *tidss = to_tidss(ddev);
	unsigned int id;
	dispc_irq_t irqstatus;

	spin_lock(&tidss->wait_lock);
	irqstatus = dispc_read_and_clear_irqstatus(tidss->dispc);
	spin_unlock(&tidss->wait_lock);

	for (id = 0; id < tidss->num_crtcs; id++) {
		struct drm_crtc *crtc = tidss->crtcs[id];
		struct tidss_crtc *tcrtc = to_tidss_crtc(crtc);
		u32 hw_videoport = tcrtc->hw_videoport;

		if (irqstatus & (DSS_IRQ_VP_VSYNC_EVEN(hw_videoport) |
				 DSS_IRQ_VP_VSYNC_ODD(hw_videoport)))
			tidss_crtc_vblank_irq(crtc);

		if (irqstatus & (DSS_IRQ_VP_FRAME_DONE(hw_videoport)))
			tidss_crtc_framedone_irq(crtc);

		if (irqstatus & DSS_IRQ_VP_SYNC_LOST(hw_videoport))
			tidss_crtc_error_irq(crtc, irqstatus);
	}

	if (irqstatus & DSS_IRQ_DEVICE_OCP_ERR)
		dev_err_ratelimited(tidss->dev, "OCP error\n");

	return IRQ_HANDLED;
}

void tidss_irq_resume(struct tidss_device *tidss)
{
	unsigned long flags;

	spin_lock_irqsave(&tidss->wait_lock, flags);
	tidss_irq_update(tidss);
	spin_unlock_irqrestore(&tidss->wait_lock, flags);
}

static void tidss_irq_preinstall(struct drm_device *ddev)
{
	struct tidss_device *tidss = to_tidss(ddev);

	spin_lock_init(&tidss->wait_lock);

	tidss_runtime_get(tidss);

	dispc_set_irqenable(tidss->dispc, 0);
	dispc_read_and_clear_irqstatus(tidss->dispc);

	tidss_runtime_put(tidss);
}

static void tidss_irq_postinstall(struct drm_device *ddev)
{
	struct tidss_device *tidss = to_tidss(ddev);
	unsigned long flags;
	unsigned int i;

	tidss_runtime_get(tidss);

	spin_lock_irqsave(&tidss->wait_lock, flags);

	tidss->irq_mask = DSS_IRQ_DEVICE_OCP_ERR;

	for (i = 0; i < tidss->num_crtcs; ++i) {
		struct tidss_crtc *tcrtc = to_tidss_crtc(tidss->crtcs[i]);

		tidss->irq_mask |= DSS_IRQ_VP_SYNC_LOST(tcrtc->hw_videoport);

		tidss->irq_mask |= DSS_IRQ_VP_FRAME_DONE(tcrtc->hw_videoport);
	}

	tidss_irq_update(tidss);

	spin_unlock_irqrestore(&tidss->wait_lock, flags);

	tidss_runtime_put(tidss);
}

int tidss_irq_install(struct drm_device *ddev, unsigned int irq)
{
	int ret;

	if (irq == IRQ_NOTCONNECTED)
		return -ENOTCONN;

	tidss_irq_preinstall(ddev);

	ret = request_irq(irq, tidss_irq_handler, 0, ddev->driver->name, ddev);
	if (ret)
		return ret;

	tidss_irq_postinstall(ddev);

	return 0;
}

void tidss_irq_uninstall(struct drm_device *ddev)
{
	struct tidss_device *tidss = to_tidss(ddev);

	tidss_runtime_get(tidss);
	dispc_set_irqenable(tidss->dispc, 0);
	tidss_runtime_put(tidss);

	free_irq(tidss->irq, ddev);
}
