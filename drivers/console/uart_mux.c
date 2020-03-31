/*
 * Copyright (c) 2020 Intel Corporation
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <logging/log.h>
LOG_MODULE_REGISTER(uart_mux, CONFIG_UART_MUX_LOG_LEVEL);

#include <sys/__assert.h>
#include <kernel.h>
#include <init.h>
#include <syscall_handler.h>
#include <device.h>
#include <drivers/uart.h>
#include <drivers/console/uart_mux.h>
#include <sys/ring_buffer.h>
#include <sys/util.h>
#include <sys/atomic.h>

#include "gsm_mux.h"

#if CONFIG_UART_MUX_DEVICE_COUNT == 0
#error "CONFIG_UART_MUX_DEVICE_COUNT tells number of DLCIs to create " \
	"and must be >0"
#endif

#define UART_MUX_WORKQ_PRIORITY CONFIG_UART_MUX_RX_PRIORITY
#define UART_MUX_WORKQ_STACK_SIZE CONFIG_UART_MUX_RX_STACK_SIZE

/* All the RX/TX data is passed via own workqueue. This is done like this
 * as the GSM modem uses global workqueue which causes difficulties if we do
 * the same here. This workqueue is shared between all the DLCI channels.
 */
K_THREAD_STACK_DEFINE(uart_mux_stack, UART_MUX_WORKQ_STACK_SIZE);
static struct k_work_q uart_mux_workq;

/* The UART mux contains information about the real UART. It will synchronize
 * the access to the real UART and pass data between it and GSM muxing API.
 * Usually there is only one instance of these in the system, if we have only
 * one UART connected to modem device.
 */
struct uart_mux {
	/* The real UART device that is shared between muxed UARTs */
	struct device *uart;

	/* GSM mux related to this UART */
	struct gsm_mux *mux;

	/* Received data is routed from ISR to MUX API via ring buffer */
	struct ring_buf *rx_ringbuf;

	/* RX worker that passes data from RX ISR to GSM mux API */
	struct k_work rx_work;

	/* Mutex for accessing the real UART */
	struct k_mutex lock;

	/* Flag that tells whether this instance is initialized or not */
	atomic_t init_done;

	/* Temporary buffer when reading data in ISR */
	u8_t rx_buf[CONFIG_UART_MUX_TEMP_BUF_SIZE];
};

#define DEFINE_UART_MUX(x, _)						\
	RING_BUF_DECLARE(uart_rx_ringbuf_##x,				\
			 CONFIG_UART_MUX_RINGBUF_SIZE);			\
	static struct uart_mux uart_mux_##x __used			\
		__attribute__((__section__(".uart_mux.data"))) = {	\
			.rx_ringbuf = &uart_rx_ringbuf_##x,		\
	};

UTIL_LISTIFY(CONFIG_UART_MUX_REAL_DEVICE_COUNT, DEFINE_UART_MUX, _)

extern struct uart_mux __uart_mux_start[];
extern struct uart_mux __uart_mux_end[];

/* UART Mux Driver Status Codes */
enum uart_mux_status_code {
	UART_MUX_UNKNOWN,      /* Initial connection status   */
	UART_MUX_CONFIGURED,   /* UART mux configuration done */
	UART_MUX_CONNECTED,    /* UART mux connected          */
	UART_MUX_DISCONNECTED, /* UART mux connection lost    */
};

struct uart_mux_config {
};

#define DEV_DATA(dev) \
	((struct uart_mux_dev_data *)(dev)->driver_data)

struct uart_mux_dev_data {
	sys_snode_t node;

	/* Configuration data */
	struct uart_mux_config cfg;

	/* This UART mux device */
	struct device *dev;

	/* The UART device where we are running on top of */
	struct uart_mux *real_uart;

	/* TX worker that will mux the transmitted data */
	struct k_work tx_work;

	/* ISR function callback worker */
	struct k_work cb_work;

	/* ISR function callback */
	uart_irq_callback_user_data_t cb;
	void *cb_user_data;

	/* Attach callback */
	uart_mux_attach_cb_t attach_cb;
	void *attach_user_data;

	/* TX data from application is handled via ring buffer */
	struct ring_buf *tx_ringbuf;

	/* Received data is routed from RX worker to application via ring
	 * buffer.
	 */
	struct ring_buf *rx_ringbuf;

	/* Muxing status */
	enum uart_mux_status_code status;

	/* DLCI (muxing virtual channel) linked to this muxed UART */
	struct gsm_dlci *dlci;

	/* Status (enabled / disabled) for RX and TX */
	bool rx_enabled : 1;
	bool tx_enabled : 1;
	bool rx_ready : 1;
	bool tx_ready : 1;
	bool in_use : 1;
};

struct uart_mux_cfg_data {
};

static sys_slist_t uart_mux_data_devlist;

static void uart_mux_cb_work(struct k_work *work)
{
	struct uart_mux_dev_data *dev_data =
		CONTAINER_OF(work, struct uart_mux_dev_data, cb_work);

	dev_data->cb(dev_data->cb_user_data);
}

static void uart_mux_rx_work(struct k_work *work)
{
	struct uart_mux *uart_mux;
	u8_t *data;
	size_t len;
	int ret;

	uart_mux = CONTAINER_OF(work, struct uart_mux, rx_work);

	/* We have now received muxed data. Push that through GSM mux API which
	 * will parse it and call proper functions to get the data to the user.
	 */

	len = ring_buf_get_claim(uart_mux->rx_ringbuf, &data,
				 CONFIG_UART_MUX_RINGBUF_SIZE);
	if (len == 0) {
		LOG_DBG("Ringbuf %p is empty!", uart_mux->rx_ringbuf);
		return;
	}

	if (IS_ENABLED(CONFIG_UART_MUX_VERBOSE_DEBUG)) {
		char tmp[sizeof("RECV muxed ") + 10];

		snprintk(tmp, sizeof(tmp), "RECV muxed %s",
			 uart_mux->uart->config->name);
		LOG_HEXDUMP_DBG(data, len, log_strdup(tmp));
	}

	gsm_mux_recv_buf(uart_mux->mux, data, len);

	ret = ring_buf_get_finish(uart_mux->rx_ringbuf, len);
	if (ret < 0) {
		LOG_DBG("Cannot flush ring buffer (%d)", ret);
	}
}

static void uart_mux_tx_work(struct k_work *work)
{
	struct uart_mux_dev_data *dev_data =
		CONTAINER_OF(work, struct uart_mux_dev_data, tx_work);
	u8_t *data;
	size_t len;

	len = ring_buf_get_claim(dev_data->tx_ringbuf, &data,
				 CONFIG_UART_MUX_RINGBUF_SIZE);
	if (!len) {
		LOG_DBG("Ringbuf %p empty!", dev_data->tx_ringbuf);
		return;
	}

	LOG_DBG("Got %d bytes from ringbuffer send to uart %p", len,
		dev_data->dev);

	if (IS_ENABLED(CONFIG_UART_MUX_VERBOSE_DEBUG)) {
		char tmp[sizeof("SEND _x") +
			 sizeof(CONFIG_UART_MUX_DEVICE_NAME)];

		snprintk(tmp, sizeof(tmp), "SEND %s",
			 dev_data->dev->config->name);
		LOG_HEXDUMP_DBG(data, len, log_strdup(tmp));
	}

	(void)gsm_dlci_send(dev_data->dlci, data, len);

	ring_buf_get_finish(dev_data->tx_ringbuf, len);
}

static int uart_mux_init(struct device *dev)
{
	struct uart_mux_dev_data *dev_data = DEV_DATA(dev);

	gsm_mux_init();

	dev_data->dev = dev;
	dev_data->real_uart = NULL; /* will be set when user attach to it */

	sys_slist_find_and_remove(&uart_mux_data_devlist, &dev_data->node);
	sys_slist_prepend(&uart_mux_data_devlist, &dev_data->node);

	k_work_init(&dev_data->tx_work, uart_mux_tx_work);
	k_work_init(&dev_data->cb_work, uart_mux_cb_work);

	LOG_DBG("Device %s dev %p dev_data %p cfg %p created",
		dev->config->name, dev, dev_data, dev->config->config_info);

	return 0;
}

/* This IRQ handler is shared between muxing UARTs. After we have received
 * data from it in uart_mux_rx_work(), we push the data to GSM mux API which
 * will call proper callbacks to pass data to correct recipient.
 */
static void uart_mux_isr(void *user_data)
{
	struct uart_mux *real_uart = user_data;
	int rx = 0;
	size_t wrote = 0;

	/* Read all data off UART, and send to RX worker for unmuxing */
	while (uart_irq_update(real_uart->uart) &&
	       uart_irq_rx_ready(real_uart->uart)) {
		rx = uart_fifo_read(real_uart->uart, real_uart->rx_buf,
				    sizeof(real_uart->rx_buf));
		if (rx <= 0) {
			continue;
		}

		wrote = ring_buf_put(real_uart->rx_ringbuf,
				     real_uart->rx_buf, rx);
		if (wrote < rx) {
			LOG_ERR("Ring buffer full, drop %d bytes",
				rx - wrote);
		}

		k_work_submit_to_queue(&uart_mux_workq, &real_uart->rx_work);
	}
}

static void uart_mux_flush_isr(struct device *dev)
{
	u8_t c;

	while (uart_fifo_read(dev, &c, 1) > 0) {
		continue;
	}
}

static void dlci_created_cb(struct gsm_dlci *dlci, bool connected,
			    void *user_data)
{
	struct uart_mux_dev_data *dev_data = user_data;

	if (connected) {
		dev_data->status = UART_MUX_CONNECTED;
	} else {
		dev_data->status = UART_MUX_DISCONNECTED;
	}

	LOG_DBG("%s %s", dev_data->dev->config->name,
		dev_data->status == UART_MUX_CONNECTED ? "connected" :
							 "disconnected");

	if (dev_data->attach_cb) {
		dev_data->attach_cb(dev_data->dev,
				    dlci ? gsm_dlci_id(dlci) : -1,
				    connected,
				    dev_data->attach_user_data);
	}
}

static int init_real_uart(struct device *mux, struct device *uart,
			  struct uart_mux **mux_uart)
{
	bool found = false;
	struct uart_mux *real_uart;

	for (real_uart = __uart_mux_start; real_uart != __uart_mux_end;
	     real_uart++) {
		if (real_uart->uart == uart) {
			found = true;
			break;
		}
	}

	if (found == false) {
		for (real_uart = __uart_mux_start; real_uart != __uart_mux_end;
		     real_uart++) {
			if (real_uart->uart == NULL) {
				real_uart->uart = uart;
				found = true;
				break;
			}
		}

		if (found == false) {
			return -ENOENT;
		}
	}

	/* Init the real UART only once */
	if (atomic_cas(&real_uart->init_done, false, true)) {
		real_uart->mux = gsm_mux_create(mux);

		LOG_DBG("Initializing UART %s and GSM mux %p",
			real_uart->uart->config->name, real_uart->mux);

		if (!real_uart->mux) {
			real_uart->uart = NULL;
			atomic_clear(&real_uart->init_done);
			return -ENOMEM;
		}

		k_work_init(&real_uart->rx_work, uart_mux_rx_work);
		k_mutex_init(&real_uart->lock);

		uart_irq_rx_disable(real_uart->uart);
		uart_irq_tx_disable(real_uart->uart);
		uart_mux_flush_isr(real_uart->uart);
		uart_irq_callback_user_data_set(
			real_uart->uart, uart_mux_isr,
			real_uart);

		uart_irq_rx_enable(real_uart->uart);
	}

	__ASSERT(real_uart->uart, "Real UART not set");

	*mux_uart = real_uart;

	return 0;
}

/* This will bind the physical (real) UART to this muxed UART */
static int attach(struct device *mux_uart, struct device *uart,
		  int dlci_address, uart_mux_attach_cb_t cb,
		  void *user_data)
{
	sys_snode_t *sn, *sns;

	if (mux_uart == NULL || uart == NULL) {
		return -EINVAL;
	}

	LOG_DBG("Attach DLCI %d (%s) to %s", dlci_address,
		mux_uart->config->name, uart->config->name);

	SYS_SLIST_FOR_EACH_NODE_SAFE(&uart_mux_data_devlist, sn, sns) {
		struct uart_mux_dev_data *dev_data =
			CONTAINER_OF(sn, struct uart_mux_dev_data, node);

		if (dev_data->dev == mux_uart) {
			struct uart_mux *real_uart;
			int ret;

			ret = init_real_uart(mux_uart, uart, &real_uart);
			if (ret < 0) {
				return ret;
			}

			dev_data->real_uart = real_uart;
			dev_data->tx_ready = true;
			dev_data->tx_enabled = true;
			dev_data->rx_enabled = true;
			dev_data->attach_cb = cb;
			dev_data->attach_user_data = user_data;
			dev_data->status = UART_MUX_CONFIGURED;

			ret = gsm_dlci_create(real_uart->mux,
					      mux_uart,
					      dlci_address,
					      dlci_created_cb,
					      dev_data,
					      &dev_data->dlci);
			if (ret < 0) {
				LOG_DBG("Cannot create DLCI %d (%d)",
					dlci_address, ret);
				return ret;
			}

			return 0;
		}
	}

	return -ENOENT;
}

static int uart_mux_poll_in(struct device *dev, unsigned char *p_char)
{
	ARG_UNUSED(dev);
	ARG_UNUSED(p_char);

	return -ENOTSUP;
}

static void uart_mux_poll_out(struct device *dev, unsigned char out_char)
{
	struct uart_mux_dev_data *dev_data = DEV_DATA(dev);

	if (dev_data->dev == NULL) {
		return;
	}

	(void)gsm_dlci_send(dev_data->dlci, &out_char, 1);
}

static int uart_mux_err_check(struct device *dev)
{
	ARG_UNUSED(dev);

	return -ENOTSUP;
}

static int uart_mux_configure(struct device *dev,
			      const struct uart_config *cfg)
{
	ARG_UNUSED(dev);
	ARG_UNUSED(cfg);

	return -ENOTSUP;
}

static int uart_mux_config_get(struct device *dev, struct uart_config *cfg)
{
	ARG_UNUSED(dev);
	ARG_UNUSED(cfg);

	return -ENOTSUP;
}

static int uart_mux_fifo_fill(struct device *dev, const u8_t *tx_data, int len)
{
	struct uart_mux_dev_data *dev_data;
	size_t wrote;

	if (dev == NULL) {
		return -EINVAL;
	}

	dev_data = DEV_DATA(dev);
	if (dev_data->dev == NULL) {
		return -ENOENT;
	}

	LOG_DBG("dev_data %p len %d tx_ringbuf space %u",
		dev_data, len, ring_buf_space_get(dev_data->tx_ringbuf));

	if (dev_data->status != UART_MUX_CONNECTED) {
		LOG_WRN("UART mux not connected, drop %d bytes", len);
		return 0;
	}

	dev_data->tx_ready = false;

	wrote = ring_buf_put(dev_data->tx_ringbuf, tx_data, len);
	if (wrote < len) {
		LOG_WRN("Ring buffer full, drop %d bytes", len - wrote);
	}

	k_work_submit_to_queue(&uart_mux_workq, &dev_data->tx_work);

	return wrote;
}

static int uart_mux_fifo_read(struct device *dev, u8_t *rx_data, const int size)
{
	struct uart_mux_dev_data *dev_data;
	u32_t len;

	if (dev == NULL) {
		return -EINVAL;
	}

	dev_data = DEV_DATA(dev);
	if (dev_data->dev == NULL) {
		return -ENOENT;
	}

	LOG_DBG("%s size %d rx_ringbuf space %u",
		dev->config->name, size,
		ring_buf_space_get(dev_data->rx_ringbuf));

	len = ring_buf_get(dev_data->rx_ringbuf, rx_data, size);

	if (ring_buf_is_empty(dev_data->rx_ringbuf)) {
		dev_data->rx_ready = false;
	}

	return len;
}

static void uart_mux_irq_tx_enable(struct device *dev)
{
	struct uart_mux_dev_data *dev_data = DEV_DATA(dev);

	if (dev_data == NULL || dev_data->dev == NULL) {
		return;
	}

	dev_data->tx_enabled = true;

	if (dev_data->cb && dev_data->tx_ready) {
		k_work_submit_to_queue(&uart_mux_workq, &dev_data->cb_work);
	}
}

static void uart_mux_irq_tx_disable(struct device *dev)
{
	struct uart_mux_dev_data *dev_data = DEV_DATA(dev);

	if (dev_data == NULL || dev_data->dev == NULL) {
		return;
	}

	dev_data->tx_enabled = false;
}

static int uart_mux_irq_tx_ready(struct device *dev)
{
	struct uart_mux_dev_data *dev_data = DEV_DATA(dev);

	if (dev_data == NULL) {
		return -EINVAL;
	}

	if (dev_data->dev == NULL) {
		return -ENOENT;
	}

	return dev_data->tx_ready;
}

static void uart_mux_irq_rx_enable(struct device *dev)
{
	struct uart_mux_dev_data *dev_data = DEV_DATA(dev);

	if (dev_data == NULL || dev_data->dev == NULL) {
		return;
	}

	dev_data->rx_enabled = true;

	if (dev_data->cb && dev_data->rx_ready) {
		k_work_submit_to_queue(&uart_mux_workq, &dev_data->cb_work);
	}
}

static void uart_mux_irq_rx_disable(struct device *dev)
{
	struct uart_mux_dev_data *dev_data = DEV_DATA(dev);

	if (dev_data == NULL || dev_data->dev == NULL) {
		return;
	}

	dev_data->rx_enabled = false;
}

static int uart_mux_irq_tx_complete(struct device *dev)
{
	ARG_UNUSED(dev);

	return -ENOTSUP;
}

static int uart_mux_irq_rx_ready(struct device *dev)
{
	struct uart_mux_dev_data *dev_data = DEV_DATA(dev);

	if (dev_data == NULL) {
		return -EINVAL;
	}

	if (dev_data->dev == NULL) {
		return -ENOENT;
	}

	return dev_data->rx_ready;
}

static void uart_mux_irq_err_enable(struct device *dev)
{
	ARG_UNUSED(dev);
}

static void uart_mux_irq_err_disable(struct device *dev)
{
	ARG_UNUSED(dev);
}

static int uart_mux_irq_is_pending(struct device *dev)
{
	struct uart_mux_dev_data *dev_data = DEV_DATA(dev);

	if (dev_data == NULL || dev_data->dev == NULL) {
		return 0;
	}

	if (dev_data->tx_ready && dev_data->tx_enabled) {
		return 1;
	}

	if (dev_data->rx_ready && dev_data->rx_enabled) {
		return 1;
	}

	return 0;
}

static int uart_mux_irq_update(struct device *dev)
{
	ARG_UNUSED(dev);

	return 1;
}

static void uart_mux_irq_callback_set(struct device *dev,
				      uart_irq_callback_user_data_t cb,
				      void *user_data)
{
	struct uart_mux_dev_data *dev_data = DEV_DATA(dev);

	if (dev_data == NULL) {
		return;
	}

	dev_data->cb = cb;
	dev_data->cb_user_data = user_data;
}

static struct uart_mux_driver_api uart_mux_driver_api = {
	.uart_api.poll_in = uart_mux_poll_in,
	.uart_api.poll_out = uart_mux_poll_out,
	.uart_api.err_check = uart_mux_err_check,
	.uart_api.configure = uart_mux_configure,
	.uart_api.config_get = uart_mux_config_get,
	.uart_api.fifo_fill = uart_mux_fifo_fill,
	.uart_api.fifo_read = uart_mux_fifo_read,
	.uart_api.irq_tx_enable = uart_mux_irq_tx_enable,
	.uart_api.irq_tx_disable = uart_mux_irq_tx_disable,
	.uart_api.irq_tx_ready = uart_mux_irq_tx_ready,
	.uart_api.irq_rx_enable = uart_mux_irq_rx_enable,
	.uart_api.irq_rx_disable = uart_mux_irq_rx_disable,
	.uart_api.irq_tx_complete = uart_mux_irq_tx_complete,
	.uart_api.irq_rx_ready = uart_mux_irq_rx_ready,
	.uart_api.irq_err_enable = uart_mux_irq_err_enable,
	.uart_api.irq_err_disable = uart_mux_irq_err_disable,
	.uart_api.irq_is_pending = uart_mux_irq_is_pending,
	.uart_api.irq_update = uart_mux_irq_update,
	.uart_api.irq_callback_set = uart_mux_irq_callback_set,

	.attach = attach,
};

struct device *uart_mux_alloc(void)
{
	sys_snode_t *sn, *sns;

	SYS_SLIST_FOR_EACH_NODE_SAFE(&uart_mux_data_devlist, sn, sns) {
		struct uart_mux_dev_data *dev_data =
			CONTAINER_OF(sn, struct uart_mux_dev_data, node);

		if (dev_data->in_use) {
			continue;
		}

		dev_data->in_use = true;

		return dev_data->dev;
	}

	return NULL;
}

struct device *z_impl_uart_mux_find(int dlci_address)
{
	sys_snode_t *sn, *sns;

	SYS_SLIST_FOR_EACH_NODE_SAFE(&uart_mux_data_devlist, sn, sns) {
		struct uart_mux_dev_data *dev_data =
			CONTAINER_OF(sn, struct uart_mux_dev_data, node);

		if (!dev_data->in_use) {
			continue;
		}

		if (dev_data->dlci == NULL) {
			continue;
		}

		if (gsm_dlci_id(dev_data->dlci) == dlci_address) {
			return dev_data->dev;
		}
	}

	return NULL;
}

int uart_mux_send(struct device *uart, const u8_t *buf, size_t size)
{
	struct uart_mux_dev_data *dev_data = DEV_DATA(uart);

	if (size == 0) {
		return 0;
	}

	if (atomic_get(&dev_data->real_uart->init_done) == false) {
		return -ENODEV;
	}

	if (IS_ENABLED(CONFIG_UART_MUX_VERBOSE_DEBUG)) {
		char tmp[sizeof("SEND muxed ") + 10];

		snprintk(tmp, sizeof(tmp), "SEND muxed %s",
			 dev_data->real_uart->uart->config->name);
		LOG_HEXDUMP_DBG(buf, size, log_strdup(tmp));
	}

	k_mutex_lock(&dev_data->real_uart->lock, K_FOREVER);

	do {
		uart_poll_out(dev_data->real_uart->uart, *buf++);
	} while (--size);

	k_mutex_unlock(&dev_data->real_uart->lock);

	return 0;
}

int uart_mux_recv(struct device *mux, struct gsm_dlci *dlci, u8_t *data,
		  size_t len)
{
	struct uart_mux_dev_data *dev_data = DEV_DATA(mux);
	size_t wrote = 0;

	LOG_DBG("%s: dlci %p data %p len %zd", mux->config->name, dlci,
		data, len);

	if (IS_ENABLED(CONFIG_UART_MUX_VERBOSE_DEBUG)) {
		char tmp[sizeof("RECV _x") +
			 sizeof(CONFIG_UART_MUX_DEVICE_NAME)];

		snprintk(tmp, sizeof(tmp), "RECV %s",
			 dev_data->dev->config->name);
		LOG_HEXDUMP_DBG(data, len, log_strdup(tmp));
	}

	wrote = ring_buf_put(dev_data->rx_ringbuf, data, len);
	if (wrote < len) {
		LOG_ERR("Ring buffer full, drop %d bytes", len - wrote);
	}

	dev_data->rx_ready = true;

	if (dev_data->cb && dev_data->rx_enabled) {
		k_work_submit_to_queue(&uart_mux_workq, &dev_data->cb_work);
	}

	return wrote;
}

#define DEFINE_UART_MUX_CFG_DATA(x, _)					  \
	struct uart_mux_cfg_data uart_mux_config_##x = {		  \
	};

#define DEFINE_UART_MUX_DEV_DATA(x, _)					  \
	RING_BUF_DECLARE(tx_ringbuf_##x, CONFIG_UART_MUX_RINGBUF_SIZE);	  \
	RING_BUF_DECLARE(rx_ringbuf_##x, CONFIG_UART_MUX_RINGBUF_SIZE);	  \
	static struct uart_mux_dev_data uart_mux_dev_data_##x = {	  \
		.tx_ringbuf = &tx_ringbuf_##x,				  \
		.rx_ringbuf = &rx_ringbuf_##x,				  \
	};

#define DEFINE_UART_MUX_DEVICE(x, _)					  \
	DEVICE_AND_API_INIT(uart_mux_##x,				  \
			    CONFIG_UART_MUX_DEVICE_NAME "_" #x,		  \
			    &uart_mux_init,				  \
			    &uart_mux_dev_data_##x,			  \
			    &uart_mux_config_##x,			  \
			    POST_KERNEL,				  \
			    CONFIG_KERNEL_INIT_PRIORITY_DEVICE,		  \
			    &uart_mux_driver_api);

UTIL_LISTIFY(CONFIG_UART_MUX_DEVICE_COUNT, DEFINE_UART_MUX_CFG_DATA, _)
UTIL_LISTIFY(CONFIG_UART_MUX_DEVICE_COUNT, DEFINE_UART_MUX_DEV_DATA, _)
UTIL_LISTIFY(CONFIG_UART_MUX_DEVICE_COUNT, DEFINE_UART_MUX_DEVICE, _)

static int init_uart_mux(struct device *device)
{
	ARG_UNUSED(device);

	k_work_q_start(&uart_mux_workq, uart_mux_stack,
		       K_THREAD_STACK_SIZEOF(uart_mux_stack),
		       K_PRIO_COOP(UART_MUX_WORKQ_PRIORITY));
	k_thread_name_set(&uart_mux_workq.thread, "uart_mux_workq");

	return 0;
}

SYS_INIT(init_uart_mux, POST_KERNEL, CONFIG_UART_MUX_INIT_PRIORITY);
