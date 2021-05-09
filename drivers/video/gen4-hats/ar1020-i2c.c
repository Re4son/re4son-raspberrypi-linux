/*
 * Microchip I2C Touchscreen Driver
 *
 * Copyright (c) 2011 Microchip Technology, Inc.
 *
 * http://www.microchip.com/mtouch
 */

/*
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 as published by
 * the Free Software Foundation.
 */

#include <linux/input.h>
#include <linux/i2c.h>
#include <linux/module.h>
#include <linux/types.h>
#include <linux/gpio.h>
#include <linux/interrupt.h>
#include <linux/irq.h>
#include <linux/kobject.h>
#include <linux/string.h>
#include <linux/sysfs.h>
#include <linux/init.h>
#include <linux/delay.h>
#include <linux/slab.h>
#include <linux/kernel.h>


// we want GPIO_30 (pin 11 on P5 pinout raspberry pi rev. 2 board)
// to generate interrupt
#define GPIO_ANY_GPIO               17

// text below will be seen in 'cat /proc/interrupt' command
#define GPIO_ANY_GPIO_DESC           "AR1021 Touch Panel Interrupt"

// below is optional, used in more complex code, in our case, this could be
// NULL
#define GPIO_ANY_GPIO_DEVICE_DESC    "AR1021 Touch Panel"


/****************************************************************************/
/* Interrupts variables block                                               */
/****************************************************************************/
//short int irq_any_gpio    = 0;


/* The maximum packet byte length */
#define MCHIP_MAX_LENGTH 5

/* The private data structure that is referenced within the I2C bus driver */
struct ar1020_i2c_priv {
	struct i2c_client *client;
	struct input_dev *input;
	struct work_struct work;
	int irq;
	int testCount;
};


/* These are all the sysfs variables used to store and retrieve information
   from a user-level application */
static char sendBuffer[100];
static char receiveBuffer[100];
static int commandMode=0;
static int commandDataPending=0;
#if 1 //default value
static int minX=0;
static int maxX=4095;
static int minY=0;
static int maxY=4095;
static int swapAxes=0;
static int invertX=0;
static int invertY=0;
#elif 0
static int minX=100;
static int maxX=3825;
static int minY=340;
static int maxY=3830;
static int swapAxes=0;
static int invertX=0;
static int invertY=1;
#elif 0
static int minX=180;
static int maxX=3725;
static int minY=320;
static int maxY=3730;
static int swapAxes=0;
static int invertX=0;
static int invertY=1;
#endif
static int lastPUCoordX=0;
static int lastPUCoordY=0;

static int lastButton=0;

/* These variables allows the IRQ to be specified via a module parameter
   or kernel parameter.  To configuration of these value, please see
   driver documentation. */
static int touchIRQ=-1;
static int probeForIRQ=0;
static int testI2Cdata=0;
static int probeMin=0;
static int probeMax=200;

module_param(touchIRQ, int, S_IRUGO);
module_param(probeMin, int, S_IRUGO);
module_param(probeMax, int, S_IRUGO);
module_param(probeForIRQ, int, S_IRUGO);
module_param(testI2Cdata, int, S_IRUGO);

extern u16 lcdpi_touchparams;

/* Since the reference to private data is stored within the I2C
   bus driver, we will store another reference within this driver
   so the sysfs related function may also access this data */
struct ar1020_i2c_priv *privRef=NULL;

/******************************************************************************
Function:
	commandDataPending_show()

Description:
	Display value of "commandDataPending" variable to application that is
	requesting it's value.
******************************************************************************/
static ssize_t commandDataPending_show(struct kobject *kobj, struct kobj_attribute *attr,
			char *buf)
{
	return sprintf(buf, "%d", commandDataPending);
}

/******************************************************************************
Function:
	commandDataPending_store()

Description:
	Save value to "commandDataPending" variable from application that is
	requesting this.
******************************************************************************/
static ssize_t commandDataPending_store(struct kobject *kobj, struct kobj_attribute *attr,
			 const char *buf, size_t count)
{
	sscanf(buf, "%d", &commandDataPending);
	return count;
}

static struct kobj_attribute commandDataPending_attribute =
    __ATTR(commandDataPending, 0660, commandDataPending_show, commandDataPending_store);


/******************************************************************************
Function:
	commandMode_show()

Description:
	Display value of "commandMode" variable to application that is
	requesting it's value.
******************************************************************************/
static ssize_t commandMode_show(struct kobject *kobj, struct kobj_attribute *attr,
			char *buf)
{
	return sprintf(buf, "%d", commandMode);
}

/******************************************************************************
Function:
	commandMode_store()

Description:
	Save value to "commandMode" variable from application that is
	requesting this.
******************************************************************************/
static ssize_t commandMode_store(struct kobject *kobj, struct kobj_attribute *attr,
			 const char *buf, size_t count)
{
	sscanf(buf, "%d", &commandMode);
	return count;

}

static struct kobj_attribute commandMode_attribute =
    __ATTR(commandMode, 0660, commandMode_show, commandMode_store);

/******************************************************************************
Function:
	receiveBuffer_show()

Description:
	Display value of "receiveBuffer" variable to application that is
	requesting it's value.
******************************************************************************/
static ssize_t receiveBuffer_show(struct kobject *kobj, struct kobj_attribute *attr,
			char *buf)
{
	/* since we have now read the receiveBuffer, receive data is no longer pending */
	commandDataPending=0;
	return sprintf(buf, "%s", receiveBuffer);
}

/******************************************************************************
Function:
	receiveBuffer_store()

Description:
	Save value to "receiveBuffer" variable from application that is
	requesting this.
******************************************************************************/
static ssize_t receiveBuffer_store(struct kobject *kobj, struct kobj_attribute *attr,
			 const char *buf, size_t count)
{
	return snprintf(receiveBuffer,sizeof(receiveBuffer),"%s",buf);
}

static struct kobj_attribute receiveBuffer_attribute =
    __ATTR(receiveBuffer, 0660, receiveBuffer_show, receiveBuffer_store);

/******************************************************************************
Function:
	sendBuffer_show()

Description:
	Display value of "sendBuffer" variable to application that is
	requesting it's value.
******************************************************************************/
static ssize_t sendBuffer_show(struct kobject *kobj, struct kobj_attribute *attr,
			char *buf)
{
	return sprintf(buf, "%s", sendBuffer);
}

/******************************************************************************
Function:
	sendBuffer_store()

Description:
	Save value to "sendBuffer" variable from application that is
	requesting this.
******************************************************************************/
static ssize_t sendBuffer_store(struct kobject *kobj, struct kobj_attribute *attr,
			 const char *buf, size_t count)
{
	int commandByte[8];
	char buff[8];
	int numCommandBytes;
	int i;

	commandDataPending=0;

	/* disallow commands to be sent until command mode is enabled */
	if (0==commandMode)
	{
		// printk("AR1020 I2C: Warning: command bytes will be ignored until commandMode is enabled\n");
		strcpy(sendBuffer,"");
		return count;
	}

	numCommandBytes=sscanf(buf,"%x %x %x %x %x %x %x %x",&commandByte[0],&commandByte[1],&commandByte[2],&commandByte[3],&commandByte[4],&commandByte[5],&commandByte[6],&commandByte[7]);


	// printk(KERN_DEBUG "AR1020 I2C: Processed %d bytes.\n",numCommandBytes);

	/* Verify command string to send to controller is valid */
	if (numCommandBytes<3)
	{
		// printk("AR1020 I2C: Insufficient command bytes to process.\n");
	}
	else if (commandByte[0]!=0x0)
	{
		// printk("AR1020 I2C: Leading zero required when sending I2C commands.\n");
	}
	else if (commandByte[1]!=0x55)
	{
		// printk("AR1020 I2C: Invalid header byte (0x55 expected).\n");
	}
	else if (commandByte[2] != (numCommandBytes-3))
	{
		// printk("AR1020 I2C: Number of command bytes specified not valid for current string.\n");
	}

	strcpy(sendBuffer,"");
	// printk(KERN_DEBUG "AR1020 I2C: sending command bytes: ");
	for (i=0;i<numCommandBytes;i++)
	{
	   buff[i]=(char)commandByte[i];
	   // printk(KERN_DEBUG "0x%02x ", commandByte[i]);

	}
	// printk(KERN_DEBUG "\n");

	i2c_master_send (privRef->client,buff,numCommandBytes);

	return snprintf(sendBuffer,sizeof(sendBuffer),"%s",buf);
}

static struct kobj_attribute sendBuffer_attribute =
    __ATTR(sendBuffer, 0660, sendBuffer_show, sendBuffer_store);

/******************************************************************************
Function:
	calibrationSettings_show()

Description:
	Display value of "calibrationSettings" variable to application that is
	requesting it's value.	The handling of the calibration settings has
	been grouped together.
******************************************************************************/
static ssize_t calibrationSettings_show(struct kobject *kobj, struct kobj_attribute *attr,
		      char *buf)
{
	int calibrationSetting=0;

	if (strcmp(attr->attr.name, "minX") == 0)
		calibrationSetting = minX;
	else if (strcmp(attr->attr.name, "maxX") == 0)
		calibrationSetting = maxX;
	else if (strcmp(attr->attr.name, "minY") == 0)
		calibrationSetting = minY;
	else if (strcmp(attr->attr.name, "maxY") == 0)
		calibrationSetting = maxY;
	else if (strcmp(attr->attr.name, "swapAxes") == 0)
		calibrationSetting = swapAxes;
	else if (strcmp(attr->attr.name, "invertX") == 0)
		calibrationSetting = invertX;
	else if (strcmp(attr->attr.name, "invertY") == 0)
		calibrationSetting = invertY;
	else if (strcmp(attr->attr.name, "lastPUCoordX") == 0)
		calibrationSetting = lastPUCoordX;
	else if (strcmp(attr->attr.name, "lastPUCoordY") == 0)
		calibrationSetting = lastPUCoordY;

	return sprintf(buf, "%d\n", calibrationSetting);
}

/******************************************************************************
Function:
	calibrationSettings_store()

Description:
	Save calibration setting to corresponding variable from application
	that is requesting this.
******************************************************************************/
static ssize_t calibrationSettings_store(struct kobject *kobj, struct kobj_attribute *attr,
		       const char *buf, size_t count)
{
	int calibrationSetting;

	sscanf(buf, "%d", &calibrationSetting);

	if (strcmp(attr->attr.name, "minX") == 0)
		minX = calibrationSetting;
	else if (strcmp(attr->attr.name, "maxX") == 0)
		maxX = calibrationSetting;
	else if (strcmp(attr->attr.name, "minY") == 0)
		minY = calibrationSetting;
	else if (strcmp(attr->attr.name, "maxY") == 0)
		maxY = calibrationSetting;
	else if (strcmp(attr->attr.name, "swapAxes") == 0)
		swapAxes = calibrationSetting;
	else if (strcmp(attr->attr.name, "invertX") == 0)
		invertX = calibrationSetting;
	else if (strcmp(attr->attr.name, "invertY") == 0)
		invertY = calibrationSetting;
	else if (strcmp(attr->attr.name, "lastPUCoordX") == 0)
		lastPUCoordX = calibrationSetting;
	else if (strcmp(attr->attr.name, "lastPUCoordY") == 0)
		lastPUCoordY = calibrationSetting;

	return count;
}

/* Defines sysfs variable associations */
static struct kobj_attribute minX_attribute =
    __ATTR(minX, 0660, calibrationSettings_show, calibrationSettings_store);
static struct kobj_attribute maxX_attribute =
    __ATTR(maxX, 0660, calibrationSettings_show, calibrationSettings_store);
static struct kobj_attribute minY_attribute =
    __ATTR(minY, 0660, calibrationSettings_show, calibrationSettings_store);
static struct kobj_attribute maxY_attribute =
    __ATTR(maxY, 0660, calibrationSettings_show, calibrationSettings_store);
static struct kobj_attribute swapAxes_attribute =
    __ATTR(swapAxes, 0660, calibrationSettings_show, calibrationSettings_store);
static struct kobj_attribute invertX_attribute =
    __ATTR(invertX, 0660, calibrationSettings_show, calibrationSettings_store);
static struct kobj_attribute invertY_attribute =
    __ATTR(invertY, 0660, calibrationSettings_show, calibrationSettings_store);
static struct kobj_attribute lastPUCoordX_attribute =
    __ATTR(lastPUCoordX, 0660, calibrationSettings_show, calibrationSettings_store);
static struct kobj_attribute lastPUCoordY_attribute =
    __ATTR(lastPUCoordY, 0660, calibrationSettings_show, calibrationSettings_store);

/*
 * Create a group of calibration attributes so we may work with them
 * as a set.
 */
static struct attribute *attrs[] = {
	&commandDataPending_attribute.attr,
	&commandMode_attribute.attr,
	&receiveBuffer_attribute.attr,
	&sendBuffer_attribute.attr,
	&minX_attribute.attr,
	&maxX_attribute.attr,
	&minY_attribute.attr,
	&maxY_attribute.attr,
	&swapAxes_attribute.attr,
	&invertX_attribute.attr,
	&invertY_attribute.attr,
	&lastPUCoordX_attribute.attr,
	&lastPUCoordY_attribute.attr,
	NULL,
};

static struct attribute_group attr_group = {
	.attrs = attrs,
};

static struct kobject *ar1020_kobj;

static irqreturn_t touch_irq_handler_func(int irq, void *dev_id);

/******************************************************************************
Function:
	decodeAR1020Packet()

Description:
	Decode packets of data from a device path using AR1XXX protocol.
	Returns 1 if a full packet is available, zero otherwise.
******************************************************************************/
int decodeAR1020Packet(struct ar1020_i2c_priv* priv, char* packet, int *index, char data)
{
	int returnValue=0;
	int x;
	int y;
	int button;
	int calX;
	int calY;

	packet[*index] = data;

	/****************************************************

	Data format, 5 bytes: SYNC, DATA1, DATA2, DATA3, DATA4

	SYNC [7:0]: 1,0,0,0,0,TOUCHSTATUS[0:0]
	DATA1[7:0]: 0,X-LOW[6:0]
	DATA2[7:0]: 0,X-HIGH[4:0]
	DATA3[7:0]: 0,Y-LOW[6:0]
	DATA4[7:0]: 0,Y-HIGH[4:0]

	TOUCHSTATUS: 0 = Touch up, 1 = Touch down

	****************************************************/

	switch ((*index)) {
		case 0:
			if (!(0x80 & data))
			{
			    /* Sync bit not set */
			    returnValue=-1;
			}
			break;

		case (MCHIP_MAX_LENGTH - 1):
			/* verify byte is valid for current index */
			if (0x80 & data)
			{
				/* byte not valid */
				break;
			}

			x = ((packet[2] & 0x1f) << 7) | (packet[1] & 0x7f);
			y= ((packet[4] & 0x1f) << 7) | (packet[3] & 0x7f);
			button = 0 != (packet[0] & 1);


			if (0==button)
			{
				lastPUCoordX=x;
				lastPUCoordY=y;
			}

			if (swapAxes)
			{
				int temp=x;
				x=y;
				y=temp;
			}


			if (invertX)
				x=4095-x;

			if (invertY)
				y=4095-y;


			if (x<minX)
				calX=0;
			else if (x>maxX)
				calX=4095;
			else
				/* percentage across calibration area times the maximum controller width */
				calX=((x-minX)*4095)/(maxX-minX);

			if (y<minY)
				calY=0;
			else if (y>maxY)
				calY=4095;
			else
				/* percentage across calibration area times the maximum controller height */
				calY=((y-minY)*4095)/(maxY-minY);

			// printk(KERN_DEBUG "AR1020 I2C: %d %d %d\n",calX,calY,button);


            if(button != lastButton) {
                input_report_key(priv->input, BTN_TOUCH, button);
                if(button)
                    input_report_abs(priv->input, ABS_PRESSURE, 15000);
                else
                    input_report_abs(priv->input, ABS_PRESSURE, 0);
                lastButton = button;
            }

            input_report_abs(priv->input, ABS_PRESSURE, 15000);
            input_report_abs(priv->input, ABS_X, calX);
            input_report_abs(priv->input, ABS_Y, calY);
			input_sync(priv->input);

			returnValue=1;
			break;
		default:
			/* verify byte is valid for current index */
			if (0x80 & data)
			{
				/* byte not valid */
				// printk("AR1020 I2C: Touch byte not valid.  Value: 0x%02x Index: 0x%02x\n",data, *index);
			}
			break;

	}

	return returnValue;
}

/******************************************************************************
Function:
	ar1020_i2c_open()

Description:
	This function is called on every attempt to open the current device
	and used for both debugging purposes fullfilling an I2C driver
	function callback requirement.
******************************************************************************/
static int ar1020_i2c_open(struct input_dev *dev)
{
	return 0;
}

/******************************************************************************
Function:
	ar1020_i2c_close()

Description:
	This function is called on every attempt to close the current device
	and used for both debugging purposes fullfilling an I2C driver
	function callback requirement.
******************************************************************************/
static void ar1020_i2c_close(struct input_dev *dev)
{
}

/******************************************************************************
Function:
	test_irq_handler_func()

Description:
	Testing to see if IRQ line of controller attached to an available
	IO line on board.
******************************************************************************/
static irqreturn_t test_irq_handler_func(int irq, void *dev_id)
{
	struct ar1020_i2c_priv *priv = (struct ar1020_i2c_priv *)dev_id;
	int err;

	if (!priv) {
        printk(KERN_ERR "AR1020 I2C:touch_irq_handler_funct: no private data\n");
		err = -EINVAL;
		return err;
	}

	priv->testCount++;

	return IRQ_NONE;
}

/******************************************************************************
Function:
	ar1020_i2c_readdata()

Description:
	When the controller interrupt is asserted, this function is scheduled
	to be called to read the controller data within the
	touch_irq_handler_func() function.
******************************************************************************/
static void ar1020_i2c_readdata(struct work_struct *work)
{
	struct ar1020_i2c_priv *priv =
		container_of(work, struct ar1020_i2c_priv, work);
	int i;
	char buff[9];

	/* We want to ensure we only read packets when we are not in the middle of command communication. Disable command mode after receiving command response to resume receiving packets. */
	if (commandMode)
	{
		commandDataPending=1;
		/* process up to 9 bytes */
		strcpy(receiveBuffer,"");

		i2c_master_recv (priv->client,buff,9);
		snprintf(receiveBuffer,sizeof(receiveBuffer),"0x%02x",0xff&buff[0]);

		if (0x55 != buff[0])
		{
			// printk("AR1020 I2C: invalid header byte\n");
		}

		snprintf(receiveBuffer,sizeof(receiveBuffer),"%s 0x%02x",receiveBuffer,0xff&buff[1]);
		if (buff[1] > 6)
		{
			// printk("AR1020 I2C: invalid byte count\n");
		}

		for (i=0;i<7;i++)
		{
			snprintf(receiveBuffer,sizeof(receiveBuffer),"%s 0x%02x",receiveBuffer,0xff&buff[i+2]);
		}

		if (buff[1]<7)
		{
			/* if command response has valid length, reformat the response */
			strcpy(receiveBuffer,"");
			for (i=0;i<buff[1]+2;i++)
			{
				snprintf(receiveBuffer,sizeof(receiveBuffer),"%s 0x%02x",receiveBuffer,0xff&buff[i]);
			}

		}

		snprintf(receiveBuffer,sizeof(receiveBuffer),"%s\n",receiveBuffer);
		// printk(KERN_DEBUG "AR1020 I2C: command response: %s",receiveBuffer);
		return;
	}



	for (i=0;i<5;i++)
	{
	  buff[i]=0;
	}

	i2c_master_recv (priv->client,buff,5);
	for (i=0;i<5;i++)
	{
	  decodeAR1020Packet(priv,buff, &i, buff[i]);
	}

}




/******************************************************************************
Function:
	ar1020_i2c_probe()

Description:
	After the kernel's platform specific source files have been modified to
	reference the "ar1020_i2c" driver, this function will then be called.
	This function needs to be called to finish registering the driver.
******************************************************************************/
//static int __devinit ar1020_i2c_probe(struct i2c_client *client,
//				      const struct i2c_device_id *id)
static int  ar1020_i2c_probe(struct i2c_client *client,
				      const struct i2c_device_id *id)
{
	struct ar1020_i2c_priv *priv=NULL;
	struct input_dev *input_dev=NULL;
	int err=0;
	int i;
	char buff[5];
	int ret;

        // printk("AR1020 I2C: ar1020_i2c_probe: begin\n");

	for (i=0;i<5;i++)
	{
		buff[i]=0;
	}

	if (!client) {
        printk(KERN_ERR "AR1020 I2C: client pointer is NULL\n");
		err = -EINVAL;
		goto error;
	}

    if (gpio_request(GPIO_ANY_GPIO, GPIO_ANY_GPIO_DESC)) {
       // printk("GPIO request faiure: %s\n", GPIO_ANY_GPIO_DESC);
       goto error;
    }

    gpio_direction_input(GPIO_ANY_GPIO);

    ret = gpio_to_irq(GPIO_ANY_GPIO);
    if(ret > 0) {
        client->irq = ret;
    }
    else {
        printk(KERN_ERR "AR1020 I2C: can not get IRQ for interrupt GPIO set for touch controller\n");
        err = -EINVAL;
        goto error;
    }

	if ((!client->irq) && (touchIRQ == -1) && (!testI2Cdata) && (!probeForIRQ)) {
        printk(KERN_ERR "AR1020 I2C: no IRQ set for touch controller\n");
		err = -EINVAL;
		goto error;
	}

    swapAxes = (lcdpi_touchparams & 0x04) >> 2;
    invertX = (lcdpi_touchparams & 0x02) >> 1;
    invertY = (lcdpi_touchparams & 0x01);


	priv = kzalloc(sizeof(struct ar1020_i2c_priv), GFP_KERNEL);
	input_dev = input_allocate_device();
	if (!priv) {
		// printk(KERN_ERR "AR1020 I2C: kzalloc error\n");
		err = -ENOMEM;
		goto error;
	}

	/* Backup pointer so sysfs helper functions may also have access to private data */
	privRef=priv;

	if (!input_dev)
	{
		// printk(KERN_ERR "AR1020 I2C: input allocate error\n");
		err = -ENOMEM;
		goto error;
	}

	priv->client = client;
	priv->irq = client->irq;
	priv->input = input_dev;

	/* Verify raw I2C data stream to ensure bus is setup correctly in the platform settings. */
	if (testI2Cdata)
	{
		// printk("AR1020 I2C: In testing mode to verify packet data.  To inhibit this mode,\n");
		// printk("unset the \"testI2Cdata\" kernel parameter.\n");
		while (1)
		{
			i2c_master_recv (priv->client,buff,5);

			/* A zero byte for first byte usually means there */
			/* is no data available in controller buffer. */
			if (0==buff[0])
			{
				continue;
			}

			for (i=0;i<5;i++)
			{
		  	 	// printk("0x%x ",buff[i]);

			}
			// printk("\n");

			if (!(0x80 & buff[0]) && (0x00 != buff[1]))
			{
				// printk("AR1020 I2C: Missing sync bit.\n");
			}
		}

	}

	/* Detect IRQ id that controller IRQ line is attached to.  This detection only works
	   if the controller's IRQ line is attached to a GPIO line configured as an input.
	   These lines are often marked as EINT (external interrupt) on the board schematic.
	   This probe assumes that I2C read communication with the controller is working
	   correctly.
	*/
	if (probeForIRQ)
	{
		// printk("AR1020 I2C: Probing for interrupt id.\n");
		// printk("AR1020 I2C: Please touch screen before IRQ probe for successful detection.\n");
		// printk("AR1020 I2C: Probing will commence in five seconds.\n\n");
		// printk("AR1020 I2C: Kernel exception messages may appear during the\n");
		// printk("AR1020 I2C: probing process.\n");

		msleep(5000);

		for (i=probeMin;i<probeMax;i++)
		{
			// printk("AR1020 I2C: Testing IRQ %d\n",i);
			priv->irq=i;

			/* set type on new handler and register gpio pin as our interrupt */
			irq_set_irq_type(i, IRQ_TYPE_EDGE_RISING);
			if (0 >= (ret=request_irq(i, test_irq_handler_func, 0, "AR1020 IRQ", priv)))
			{
				priv->testCount=0;

				/* read I2C data to ensure IRQ lines is not asserted */
				i2c_master_recv (priv->client,buff,5);

				msleep(1000);
				if (ret>=0)
				{
					free_irq(i, priv);
				}

				/* successful detection if count within this range */
				if ((priv->testCount > 0) && (priv->testCount < 3))
				{
					// printk("AR1020 I2C: Touch IRQ detected at ID: %d.\n",i);
					priv->irq=i;
					break;
				}
			}
			else
			{
				// printk("AR1020 I2C: IRQ %d not available.\n", i);
			}
	  	}
		if (i==probeMax)
		{
			// printk("AR1020 I2C: Touch IRQ not detected. Using IRQ %d.\n",priv->irq);
        }

	}
	/* Use default settings */
	else if (touchIRQ == -1)
	{
		// printk("AR1020 I2C: Using IRQ %d set via board's platform setting.\n", priv->irq);
	}
	else
	{
		// printk("AR1020 I2C: Using IRQ %d set via kernel parameter.\n", touchIRQ);
		priv->irq=touchIRQ;
	}

	INIT_WORK(&priv->work, ar1020_i2c_readdata);

	input_dev->name = "AR1020 Touchscreen";
	input_dev->id.bustype = BUS_I2C;

	input_dev->open = ar1020_i2c_open;
	input_dev->close = ar1020_i2c_close;

	input_dev->evbit[0] = BIT_MASK(EV_KEY) | BIT_MASK(EV_ABS);
	input_dev->keybit[BIT_WORD(BTN_TOUCH)] = BIT_MASK(BTN_TOUCH);

	input_set_abs_params(input_dev, ABS_X, 0, 4095, 0, 0);
	input_set_abs_params(input_dev, ABS_Y, 0, 4095, 0, 0);
    input_set_abs_params(input_dev, ABS_PRESSURE, 0, 15000, 0, 0);
	err = input_register_device(input_dev);
	if (err)
	{
		// printk(KERN_ERR "AR1020 I2C: error registering input device\n");
		goto error;
	}

	/* set type and register gpio pin as our interrupt */
	irq_set_irq_type(priv->irq, IRQ_TYPE_EDGE_RISING);
	err = request_irq(priv->irq, touch_irq_handler_func, 0, "AR1020 I2C IRQ", priv);

	i2c_set_clientdata(client, priv);

	return 0;

 error:

	if (input_dev)
		input_free_device(input_dev);

	if (priv)
		kfree(priv);

	return err;

}

/******************************************************************************
Function:
	ar1020_i2c_remove()

Description:
	Unregister/remove the kernel driver from memory.
******************************************************************************/
static int ar1020_i2c_remove(struct i2c_client *client)
{
	struct ar1020_i2c_priv *priv = (struct ar1020_i2c_priv *)i2c_get_clientdata(client);

    gpio_free(GPIO_ANY_GPIO);
    free_irq(priv->irq, priv);
	input_unregister_device(priv->input);
	kfree(priv);

	return 0;
}

/* This structure describe a list of supported slave chips */
static const struct i2c_device_id ar1020_i2c_id[] = {
	{ "ar1020_i2c", 0 },
	{ }
};

/******************************************************************************
Function:
	touch_irq_handler_func()

Description:
	After the interrupt is asserted for the controller, this
	is the first function that is called.  Since this is a time sensitive
	function, we need to immediately schedule work so the integrity of
	properly system operation

	This function needs to be called to finish registering the driver.
******************************************************************************/
static irqreturn_t touch_irq_handler_func(int irq, void *dev_id)
{
	struct ar1020_i2c_priv *priv = (struct ar1020_i2c_priv *)dev_id;
	char buff[5];
	int i;
	int err;
	for (i=0;i<5;i++)
	{
		buff[i]=0;
	}

	// printk(KERN_NOTICE "AR1020 I2C: Interrupt Service rutine\n");

	if (!priv) {
        printk(KERN_ERR "AR1020 I2C: touch_irq_handler_funct: no private data\n");
		err = -EINVAL;
		return err;
	}

	 /* delegate I2C transactions since hardware interupts need to be handled very fast */
	schedule_work(&priv->work);

	return IRQ_HANDLED;
}

static const struct of_device_id ar1020_dt_ids[] = {
    { .compatible = "microchip,ar10204d" },
    {},
};

MODULE_DEVICE_TABLE(of, ar1020_dt_ids);


/* This is the initial set of information information the kernel has
   before probing drivers on the system, */
static struct i2c_driver ar1020_i2c_driver = {
	.driver = {
		.name	= "ar1020_i2c",
        .of_match_table = of_match_ptr(ar1020_dt_ids),
	},
	.probe		= ar1020_i2c_probe,
	.remove		= ar1020_i2c_remove,
	/* suspend/resume functions not needed since controller automatically
  	   put's itself to sleep mode after configurable short period of time */
    //.suspend	= NULL,
    //.resume		= NULL,
	.id_table	= ar1020_i2c_id,
};



/****************************************************************************/
/* This function configures interrupts.                                     */
/****************************************************************************/
void r_int_config(void) {

   if (gpio_request(GPIO_ANY_GPIO, GPIO_ANY_GPIO_DESC)) {
      // printk("GPIO request faiure: %s\n", GPIO_ANY_GPIO_DESC);
      return;
   }

   gpio_direction_input(GPIO_ANY_GPIO);

//   if ( (irq_any_gpio = gpio_to_irq(GPIO_ANY_GPIO)) < 0 ) {
   if ( (touchIRQ = gpio_to_irq(GPIO_ANY_GPIO)) < 0 ) {
      // printk("GPIO to IRQ mapping faiure %s\n", GPIO_ANY_GPIO_DESC);
      return;
   }

   //// printk(KERN_NOTICE "Mapped int %d\n", irq_any_gpio);
   // printk(KERN_NOTICE "Mapped int %d\n", touchIRQ);
#if 0
   if (request_irq(touchIRQ,
                   (irq_handler_t ) touch_irq_handler_func,
                   IRQF_TRIGGER_RISING,
                   GPIO_ANY_GPIO_DESC,
                   GPIO_ANY_GPIO_DEVICE_DESC)) {
      // printk("Irq Request failure\n");
      return;
   }
#endif
   return;
}


/****************************************************************************/
/* This function releases interrupts.                                       */
/****************************************************************************/
void r_int_release(void) {

   //free_irq(irq_any_gpio, GPIO_ANY_GPIO_DEVICE_DESC);
   //free_irq(touchIRQ, GPIO_ANY_GPIO_DEVICE_DESC);
   gpio_free(GPIO_ANY_GPIO);

   return;
}

/******************************************************************************
Function:
	ar1020_i2c_init()

Description:
	This function is called during startup even if the platform specific
	files have not been setup yet.
******************************************************************************/
static int __init ar1020_i2c_init(void)
{
	int retval;
        // printk("AR1020 I2C: ar1020_i2c_init: begin\n");
	strcpy(receiveBuffer,"");
	strcpy(sendBuffer,"");

	/*
	 * Creates a kobject "ar1020" that appears as a sub-directory
	 * under "/sys/kernel".
	 */
	ar1020_kobj = kobject_create_and_add("ar1020", kernel_kobj);
	if (!ar1020_kobj)
	{
		// printk(KERN_ERR "AR1020 I2C: cannot create kobject\n");
		return -ENOMEM;
	}

	/* Create the files associated with this kobject */
	retval = sysfs_create_group(ar1020_kobj, &attr_group);
	if (retval)
	{
		// printk(KERN_ERR "AR1020 I2C: error registering ar1020-i2c driver's sysfs interface\n");
		kobject_put(ar1020_kobj);
	}

	retval = i2c_add_driver(&ar1020_i2c_driver);

    //r_int_config();

	return retval;
}

/******************************************************************************
Function:
	ar1020_i2c_exit()

Description:
	This function is called after ar1020_i2c_remove() immediately before
	being removed from the kernel.
******************************************************************************/
static void __exit ar1020_i2c_exit(void)
{

    //r_int_release();
	// printk("AR1020 I2C: ar1020_i2c_exit begin\n");
	kobject_put(ar1020_kobj);
	i2c_del_driver(&ar1020_i2c_driver);
}

MODULE_AUTHOR("Steve Grahovac <steve.grahovac@microchip.com>");
MODULE_DESCRIPTION("AR1020 touchscreen I2C bus driver");
MODULE_LICENSE("GPL");

/* Enable the ar1020_i2c_init() to be run by the kernel during initialization */
module_init(ar1020_i2c_init);

/* Enables the ar1020_i2c_exit() to be called during cleanup.  This only
has an effect if the driver is compiled as a kernel module. */
module_exit(ar1020_i2c_exit);

