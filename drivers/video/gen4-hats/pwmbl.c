/*
 * Raspberry Pi speciffic PWM backlight driver for 4D Systems 4DPi display modules
 * PWM an be controlled by a timer or a chain of DMA transfers 
 * (c) Andrej Strancar 2015
 * 
 * based gpio_backlight.c driver by Laurent Pinchart
 *  - added high resolution timer PWM
 *  - added two versions of DMA-PWM based on pi-blaster driver by Thomas Sarlandie
 *
 * gpio_backlight.c - Simple GPIO-controlled backlight
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#include <linux/version.h>
#include <linux/backlight.h>
#include <linux/err.h>
#include <linux/fb.h>
#include <linux/gpio.h>
#include <linux/interrupt.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/platform_data/gpio_backlight.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <linux/delay.h>
#include <linux/dmaengine.h>

#include <linux/hrtimer.h>
#include <linux/ktime.h>

//#include <mach/platform.h>
#ifdef CONFIG_ARCH_MULTI_V7
#define BCM2708_PERI_BASE 0x3F000000
#else
#define BCM2708_PERI_BASE 0x20000000
#endif
#define GPIO_BASE                (BCM2708_PERI_BASE + 0x200000) /* GPIO controller */
#define DMA_BASE                 (BCM2708_PERI_BASE + 0x7000) 	/* DMA controller */
  
#if (LINUX_VERSION_CODE < KERNEL_VERSION(4,0,0))
#include <mach/dma.h>
#else
#include <linux/platform_data/dma-bcm2708.h>
#endif

#include <linux/dma-mapping.h>
#include <linux/io.h>

/* macro for bcm2835 peripherals DMA access*/
#define DMA_PERIPHERAL_ADDRESS(x) ((x) & 0x00ffffff) | 0x7e000000;

#define DMA_DELAY_PWM 0
#define DMA_DELAY_PCM 1

#define GPIO_LEN		0xb4
#define DMA_LEN			(0x100 * 15) //0x24
#define DMA_CHAN_SIZE 0x100
//#define DMA_LEN			0x24
#define PWM_BASE		(BCM2708_PERI_BASE + 0x20C000)
#define PWM_LEN			0x28
#define CLK_BASE		(BCM2708_PERI_BASE + 0x101000)
#define CLK_LEN			0xA8
#define PCM_BASE 		(BCM2708_PERI_BASE + 0x203000)
#define PCM_LEN			0x24

#define GPFSEL0			(0x00/4)
#define GPFSEL1			(0x04/4)
#define GPSET0			(0x1c/4)
#define GPCLR0			(0x28/4)
#define GPSET1			(0x20/4)
#define GPCLR1			(0x2c/4)

#define PWM_CTL			(0x00/4)
#define PWM_STA			(0x04/4)
#define PWM_DMAC		(0x08/4)
#define PWM_RNG1		(0x10/4)
#define PWM_FIFO		(0x18/4)

#define PWMCLK_CNTL		40
#define PWMCLK_DIV		41

#define PWMCTL_MODE1	(1<<1)
#define PWMCTL_PWEN1	(1<<0)
#define PWMCTL_CLRF		(1<<6)
#define PWMCTL_USEF1	(1<<5)

#define PWMDMAC_ENAB	(1<<31)
// I think this means it requests as soon as there is one free slot in the FIFO
// which is what we want as burst DMA would mess up our timing..
#define PWMDMAC_THRSHLD	((15<<8)|(15<<0))

#define PCM_CS_A		(0x00/4)
#define PCM_FIFO_A		(0x04/4)
#define PCM_MODE_A		(0x08/4)
#define PCM_RXC_A		(0x0c/4)
#define PCM_TXC_A		(0x10/4)
#define PCM_DREQ_A		(0x14/4)
#define PCM_INTEN_A		(0x18/4)
#define PCM_INT_STC_A		(0x1c/4)
#define PCM_GRAY		(0x20/4)

#define PCMCLK_CNTL		38
#define PCMCLK_DIV		39

#define DMA_CS			(BCM2708_DMA_CS/4)
#define DMA_CONBLK_AD	(BCM2708_DMA_ADDR/4)
#define DMA_DEBUG		(BCM2708_DMA_DEBUG/4)

#define BCM2708_DMA_END				(1<<1)	// Why is this not in mach/dma.h ?
#define BCM2708_DMA_NO_WIDE_BURSTS	(1<<26)

#define BLANK		1
#define UNBLANK		0

struct gpio_backlight {
    struct device *dev;
    struct device *fbdev;

    int gpio;
    int active;
};

struct bl_trig_notifier {
    int brightness;
    int old_status;
    struct notifier_block notifier;
    unsigned invert;
};

struct ctldata_s {
    struct bcm2708_dma_cb cb[4];	// gpio-hi, delay, gpio-lo, delay, for each servo output
    uint32_t gpiodata;				// set-pin, clear-pin values, per servo output
    uint32_t pwmdata;				// the word we write to the pwm fifo
};

struct {
    void *buf;
    dma_addr_t dma;
} ctl_page;

static volatile uint32_t *gpio_reg;
static volatile uint32_t *dma_reg;
void __iomem *dma_chan_base;
int dma_chan;
int dma_irq;
static volatile uint32_t *clk_reg;
static volatile uint32_t *pwm_reg;
static volatile uint32_t *pcm_reg;

struct backlight_device *bl;
u8 cnt;

u8 gpios_set_regs[] = {GPSET0, GPSET1};
u8 gpios_clr_regs[] = {GPCLR0, GPCLR1};

static struct ctldata_s *ctl, *ctl_dma;
static int cycle_ticks = 2000;
static int tick_scale = 6;

unsigned long timer_interval_ns = 100000;
static struct hrtimer hr_timer;

static ushort pwmbl_brightness = 100;
static const char *pwmbl_title;

static int pwm_stopped = 0;

/* module parameters */
static ushort pwm = 2;
module_param(pwm, ushort, 0444);
MODULE_PARM_DESC(pwm, "backlight PWM type: 0-soft_PWM, 1-DMA_PWM, 2-DMA_PCM");

static ushort gpio = 22;
module_param(gpio, ushort, 0444);
MODULE_PARM_DESC(gpio, "GPIO used for PWM: 0-63");

#if (LINUX_VERSION_CODE >= KERNEL_VERSION(4,1,0))
static ushort dma_channel = 14;
module_param(dma_channel, ushort, 0444);
MODULE_PARM_DESC(dma_channel, "DMA channel for DMA based pwm (0-14)");
#endif
static int gpio_backlight_update_status(struct backlight_device *bl) {
    struct gpio_backlight *gbl = bl_get_data(bl);
    int brightness = bl->props.brightness;
    ktime_t ktime;

//	printk("blank=%d\n", bl->props.fb_blank);
//	if (bl->props.power != FB_BLANK_UNBLANK
//			|| bl->props.fb_blank != FB_BLANK_UNBLANK
//			|| bl->props.state & (BL_CORE_SUSPENDED | BL_CORE_FBBLANK))
    if(bl->props.fb_blank != 0)
        brightness = 0;

    if (!pwm) {  // software PWM
        switch(brightness) {
        case 100:
        case 0:
            mdelay(20); // wait for the timer to stop
            gpio_set_value(gbl->gpio, brightness ? gbl->active : !gbl->active);
            pwm_stopped = 1;
            break;

        default:
            if(pwm_stopped) {
                pwm_stopped = 0;
                ktime = ktime_set( 0, timer_interval_ns );
                hrtimer_start( &hr_timer, ktime, HRTIMER_MODE_REL );
            }
            break;
        }
    } else { 		// HW PWM
        switch(brightness) {
        case 100:
            gpio_set_value(gbl->gpio, brightness ? gbl->active : !gbl->active);
            ctl->cb[0].next = 0;
            pwm_stopped = 1;
            break;

        case 0:
            gpio_set_value(gbl->gpio, brightness ? gbl->active : !gbl->active);
            ctl->cb[3].next = 0;
            pwm_stopped = 1;
            break;

        default:
            if(pwm_stopped) {
                pwm_stopped = 0;
                ctl->cb[0].next = (uint32_t)(ctl_dma->cb + 1);
                ctl->cb[3].next = (uint32_t)(ctl_dma->cb);
                dma_reg[DMA_CS] = BCM2708_DMA_RESET;
                udelay(10);
                dma_reg[DMA_CS] = BCM2708_DMA_INT | BCM2708_DMA_END;
                dma_reg[DMA_CONBLK_AD] = (uint32_t)(ctl_dma->cb);
                dma_reg[DMA_DEBUG] = 7; // clear debug error flags
                udelay(10);
                dma_reg[DMA_CS] = 0x10880001;	// go, mid priority, wait for outstanding writes
            }
        }

        if((brightness !=0) && (brightness != 100)) {
            switch(pwm) {
            case 1:	// DMA-PWM
                // on delay;
                ctl->cb[1].length = (brightness) * 3;
                // off delay;
                ctl->cb[3].length = (100 - brightness ) * 3;
                break;

            case 2: // DMA-PCM
                // on delay;
                ctl->cb[1].length = (brightness) * 5;
                // off delay;
                ctl->cb[3].length = (100 - brightness ) * 5;
                break;
            }
        }
        //printk("%d/%d", ctl->cb[1].length, ctl->cb[3].length)
    }

    return 0;
}

static int gpio_backlight_get_brightness(struct backlight_device *bl) {
    return bl->props.brightness;
}

static int gpio_backlight_check_fb(struct backlight_device *bl,
        struct fb_info *info) {
    struct gpio_backlight *gbl = bl_get_data(bl);

    return gbl->fbdev == NULL || gbl->fbdev == info->dev;
}

static int fb_notifier_callback(struct notifier_block *p,
                unsigned long event, void *data)
{
    struct fb_event *fb_event = data;
    int *blank;

//	printk("event= %ld\n", event);¸

    /* If we aren't interested in this event, skip it immediately ... */
    if (event != FB_EVENT_BLANK)
    // if((event != FB_EVENT_CONBLANK) && (event != FB_EARLY_EVENT_BLANK) && (event != FB_R_EARLY_EVENT_BLANK))
        return 0;

    blank = fb_event->data;
    bl->props.fb_blank = *blank;
    gpio_backlight_update_status(bl);

    return notifier_from_errno(0);
}

static struct notifier_block nb = { &fb_notifier_callback, NULL, 0 };

#define SAMPLE_US 10

int init_dma_pwm(struct platform_device *pdev, u8 delay_hw)
{
    int ret;

    ctl_page.buf = dma_alloc_coherent(&pdev->dev, 4096, &ctl_page.dma, GFP_DMA);

    ctl = (struct ctldata_s *)ctl_page.buf;
    ctl_dma = (struct ctldata_s *)ctl_page.dma;

#if (LINUX_VERSION_CODE < KERNEL_VERSION(4,1,0))
    /* register DMA channel */
    ret = bcm_dma_chan_alloc(BCM_DMA_FEATURE_NORMAL_ORD, &dma_chan_base, &dma_irq);
    if (ret < 0) {
        printk("couldn't allocate a DMA channel\n");
        //return ret;
    }
    dma_chan = ret;
    printk("DMA channel %d at address 0x%08lx with irq %d\n",
                          dma_chan, (unsigned long)dma_chan_base, dma_irq);
#else
    dma_chan = dma_channel;
    dev_info(&pdev->dev, "Using DMA channel %d\n",
         dma_chan);
#endif

    gpio_reg = (uint32_t *)ioremap(GPIO_BASE, GPIO_LEN);

#if (LINUX_VERSION_CODE < KERNEL_VERSION(4,1,0))
    dma_reg = (uint32_t *)dma_chan_base;
#else
    dma_reg  = (uint32_t *)ioremap(DMA_BASE,  DMA_LEN) + dma_chan * (DMA_CHAN_SIZE / sizeof(dma_reg));
#endif

    clk_reg  = (uint32_t *)ioremap(CLK_BASE,  CLK_LEN);
    pwm_reg  = (uint32_t *)ioremap(PWM_BASE,  PWM_LEN);
    pcm_reg  = (uint32_t *)ioremap(PCM_BASE, PCM_LEN);

    memset(ctl, 0, sizeof(*ctl));

    // Build the DMA CB chain
    // Set gpio high
    ctl->gpiodata = 1 << (gpio % 32);

    ctl->cb[0].info   = BCM2708_DMA_NO_WIDE_BURSTS | BCM2708_DMA_WAIT_RESP;
    ctl->cb[0].src    = (uint32_t)(&ctl_dma->gpiodata);
    ctl->cb[0].dst    = DMA_PERIPHERAL_ADDRESS(GPIO_BASE + gpios_set_regs[gpio / 32]*4);
    ctl->cb[0].length = sizeof(uint32_t);
    ctl->cb[0].stride = 0;
    ctl->cb[0].next = (uint32_t)(ctl_dma->cb + 1);

    // delay
    ctl->cb[1].src    = (uint32_t)(&ctl_dma->pwmdata);
    if(delay_hw == DMA_DELAY_PCM) {	// PCM DELAY
        ctl->cb[1].info   = BCM2708_DMA_NO_WIDE_BURSTS | BCM2708_DMA_WAIT_RESP | BCM2708_DMA_D_DREQ | BCM2708_DMA_PER_MAP(2);
         ctl->cb[1].dst    = DMA_PERIPHERAL_ADDRESS(PCM_BASE + PCM_FIFO_A*4);
    } else { 			// PWM DELAY
        ctl->cb[1].info   = BCM2708_DMA_NO_WIDE_BURSTS | BCM2708_DMA_WAIT_RESP | BCM2708_DMA_D_DREQ | BCM2708_DMA_PER_MAP(5);
        ctl->cb[1].dst    = DMA_PERIPHERAL_ADDRESS(PWM_BASE + PWM_FIFO*4);
    }
    ctl->cb[1].length = sizeof(uint32_t) * (cycle_ticks / 8 - 1);
    ctl->cb[1].stride = 0;
    ctl->cb[1].next = (uint32_t)(ctl_dma->cb + 2);

    // Set gpio lo
    ctl->cb[2].info   = BCM2708_DMA_NO_WIDE_BURSTS | BCM2708_DMA_WAIT_RESP;
    ctl->cb[2].src    = (uint32_t)(&ctl->gpiodata) & 0x7fffffff;
    ctl->cb[2].dst    = DMA_PERIPHERAL_ADDRESS(GPIO_BASE + gpios_clr_regs[gpio / 32]*4);
    ctl->cb[2].length = sizeof(uint32_t);
    ctl->cb[2].stride = 0;
    ctl->cb[2].next = (uint32_t)(ctl_dma->cb + 3);

    // delay
    ctl->cb[3].src    = (uint32_t)(&ctl_dma->pwmdata);
    if(delay_hw == DMA_DELAY_PCM) {	// PCM DELAY
        ctl->cb[3].info   = BCM2708_DMA_NO_WIDE_BURSTS | BCM2708_DMA_WAIT_RESP | BCM2708_DMA_D_DREQ | BCM2708_DMA_PER_MAP(2);
        ctl->cb[3].dst    = DMA_PERIPHERAL_ADDRESS(PCM_BASE + PCM_FIFO_A*4);
    } else { 			// PWM DELAY
        ctl->cb[3].info   = BCM2708_DMA_NO_WIDE_BURSTS | BCM2708_DMA_WAIT_RESP | BCM2708_DMA_D_DREQ | BCM2708_DMA_PER_MAP(5);
        ctl->cb[3].dst    = DMA_PERIPHERAL_ADDRESS(PWM_BASE + PWM_FIFO*4);
    }
    ctl->cb[3].length = sizeof(uint32_t) * (cycle_ticks / 8 - 1);
    ctl->cb[3].stride = 0;
    // Point last cb back to first one so it loops continuously
    ctl->cb[3].next = (uint32_t)(ctl_dma->cb);


    if(delay_hw == DMA_DELAY_PWM ) {	// Initialise PWM
        pwm_reg[PWM_CTL] = 0;
        udelay(10);
        clk_reg[PWMCLK_CNTL] = 0x5A000006;		// Source=PLLD (500MHz)
        udelay(100);
        clk_reg[PWMCLK_DIV] = 0x5A000000 | (500<<12);	// set pwm div to 500, giving 1MHz
        udelay(100);
        clk_reg[PWMCLK_CNTL] = 0x5A000016;		// Source=PLLD and enable
        udelay(100);
        pwm_reg[PWM_RNG1] = SAMPLE_US * 10;
        udelay(10);
        pwm_reg[PWM_DMAC] = PWMDMAC_ENAB | PWMDMAC_THRSHLD;
        udelay(10);
        pwm_reg[PWM_CTL] = PWMCTL_CLRF;
        udelay(10);
        pwm_reg[PWM_CTL] = PWMCTL_USEF1 | PWMCTL_PWEN1;
    } else { // Initialise PCM
        pcm_reg[PCM_CS_A] = 1;				// Disable Rx+Tx, Enable PCM block
        udelay(100);
        clk_reg[PCMCLK_CNTL] = 0x5A000006;		// Source=PLLD (500MHz)
        udelay(100);
        clk_reg[PCMCLK_DIV] = 0x5A000000 | (500<<12);	// Set pcm div to 500, giving 1MHz
        udelay(100);
        clk_reg[PCMCLK_CNTL] = 0x5A000016;		// Source=PLLD and enable
        udelay(100);

        pcm_reg[PCM_TXC_A] = 0<<31 | 1<<30 | 0<<20 | 0<<16; // 1 channel, 8 bits
        udelay(100);
        pcm_reg[PCM_MODE_A] = (tick_scale * 10 - 1) << 10;
        udelay(100);
        pcm_reg[PCM_CS_A] |= 1<<4 | 1<<3;		// Clear FIFOs
        udelay(100);
        pcm_reg[PCM_DREQ_A] = 64<<24 | 64<<8;		// DMA Req when one slot is free?
        udelay(100);
        pcm_reg[PCM_CS_A] |= 1<<9;			// Enable DMA
        udelay(100);
        pcm_reg[PCM_CS_A] |= 1 << 2;
    }

    // Initialise the DMA
    dma_reg[DMA_CS] = BCM2708_DMA_RESET;
    udelay(10);
    dma_reg[DMA_CS] = BCM2708_DMA_INT | BCM2708_DMA_END;
    //dma_reg[DMA_CONBLK_AD] = (uint32_t)(ctl_dma->cb);
    dma_reg[DMA_DEBUG] = 7; // clear debug error flags
    udelay(10);
    dma_reg[DMA_CS] = 0x10880001;	// go, mid priority, wait for outstanding writes

    return 0;
}

enum hrtimer_restart timer_callback( struct hrtimer *timer_for_restart )
{
    ktime_t currtime , interval;
    int brightness = bl->props.brightness;

    if(bl->props.fb_blank != 0)
        brightness = 0;

    currtime  = ktime_get();
    interval = ktime_set(0,timer_interval_ns);
    gpio_set_value(40,(cnt++ & 1));
    if(cnt & 1)
        interval = ktime_set(0, (100 - brightness) * timer_interval_ns);
    else
        interval = ktime_set(0, brightness * timer_interval_ns);

    hrtimer_forward(timer_for_restart, currtime, interval);

    if((brightness != 0) && (brightness != 100))
        return HRTIMER_RESTART;

    return HRTIMER_NORESTART;
}

static int timer_init(void) {
    ktime_t ktime;
    ktime = ktime_set( 0, timer_interval_ns );
    hrtimer_init( &hr_timer, CLOCK_MONOTONIC, HRTIMER_MODE_REL );
    hr_timer.function = &timer_callback;
    hrtimer_start( &hr_timer, ktime, HRTIMER_MODE_REL );
    return 0;
}

static const struct backlight_ops gpio_backlight_ops = { .options =
        BL_CORE_SUSPENDRESUME, .update_status = gpio_backlight_update_status,
        .get_brightness = gpio_backlight_get_brightness, .check_fb =
                gpio_backlight_check_fb, };

static int gpio_backlight_parse_dt(struct device *dev)
{
    struct device_node *node = dev->of_node;

    if(!node)
        return -1;

    of_property_read_u16(node, "4d,pwmbl-gpio", (u16*)&gpio);
    of_property_read_u16(node, "4d,pwmbl-type", (u16*)&pwm);
    of_property_read_u16(node, "4d,pwmbl-brightness", (u16*)&pwmbl_brightness);
    of_property_read_string(node, "4d,pwmbl-title", &pwmbl_title);

    return 0;
}

static int gpio_backlight_probe(struct platform_device *pdev) {
        struct backlight_properties props = { 100, 100, };
        struct gpio_backlight *gbl;
        int ret;

        gpio_backlight_parse_dt(&pdev->dev);

        gbl = devm_kzalloc(&pdev->dev, sizeof(*gbl), GFP_KERNEL);
        if (gbl == NULL)
            return -ENOMEM;

        gbl->dev = &pdev->dev;
        gbl->fbdev = NULL;
        gbl->gpio = gpio;
        gbl->active = 1;

        ret = devm_gpio_request_one(gbl->dev, gbl->gpio,
                GPIOF_DIR_OUT | (gbl->active ? GPIOF_INIT_HIGH : GPIOF_INIT_LOW),
                "gpio-backlight");
        if (ret < 0) {
            dev_err(&pdev->dev, "unable to request GPIO\n");
            return ret;
        }

        props.type = BACKLIGHT_RAW;
        props.power = FB_BLANK_POWERDOWN;
        props.brightness = 10;

        if(pwmbl_title)
            bl = backlight_device_register(pwmbl_title, &pdev->dev, gbl, &gpio_backlight_ops, &props);
        else
            bl = backlight_device_register(dev_name(&pdev->dev), &pdev->dev, gbl, &gpio_backlight_ops, &props);

        if (IS_ERR(bl)) {
            dev_err(&pdev->dev, "failed to register backlight\n");
            return PTR_ERR(bl);
        }

    switch(pwm) {
    case 0:
        timer_init();
        break;

    case 1:
        init_dma_pwm(pdev, DMA_DELAY_PWM);
        gpio_set_value(gbl->gpio, gbl->active);
        ctl->cb[0].next = 0;
        pwm_stopped = 1;
        break;

    case 2:
        init_dma_pwm(pdev, DMA_DELAY_PCM);
        gpio_set_value(gbl->gpio, gbl->active);
        ctl->cb[0].next = 0;
        pwm_stopped = 1;
        break;

    }

    ret = fb_register_client(&nb);
    if (ret)
        dev_err(&pdev->dev, "unable to register backlight trigger\n");

    bl->props.brightness = pwmbl_brightness;
    backlight_update_status(bl);

    platform_set_drvdata(pdev, bl);
    return 0;
}

static int gpio_backlight_remove(struct platform_device *pdev) {
    struct backlight_device *bl = platform_get_drvdata(pdev);
    int ret;

    backlight_device_unregister(bl);

    if (pwm) {
        ctl->cb[0].next = 0;
        mdelay(200);
        dma_reg[DMA_CS] = BCM2708_DMA_RESET;
        pwm_reg[PWM_CTL] = 0;
        udelay(10);
        free_pages((unsigned long)ctl, 0);

        if(ctl_page.buf)
            dma_free_coherent(&pdev->dev, 4096, ctl_page.buf, ctl_page.dma);

        iounmap(gpio_reg);
#if (LINUX_VERSION_CODE < KERNEL_VERSION(4,1,0))
        bcm_dma_chan_free(dma_chan);
#endif
        iounmap(clk_reg);
        iounmap(pwm_reg);
    } else {
        hrtimer_cancel( &hr_timer );
    }

    ret = fb_unregister_client(&nb);
        if (ret)
            dev_err(&pdev->dev, "unable to unregister backlight trigger\n");

    return 0;
}

static const struct of_device_id pwmbl_dt_ids[] = {
    { .compatible = "4dsystems,dma-pwm" },
    {},
};

MODULE_DEVICE_TABLE(of, pwmbl_dt_ids);

static struct platform_driver gpio_backlight_driver = { .driver = { .name =
        "pwm-backlight", .owner = THIS_MODULE, .of_match_table = of_match_ptr(pwmbl_dt_ids),},
        .probe = gpio_backlight_probe, .remove = gpio_backlight_remove, };

module_platform_driver( gpio_backlight_driver);

//MODULE_AUTHOR("Laurent Pinchart <laurent.pinchart@ideasonboard.com>");
MODULE_DESCRIPTION("GPIO-based PWM Backlight Driver");
MODULE_LICENSE("GPL");
MODULE_ALIAS("platform:gpio-backlight");