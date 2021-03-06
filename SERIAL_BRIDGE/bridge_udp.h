#pragma once

#include <ESPAsyncWebServer.h>
#include "esphome/core/component.h"
#include "esphome/core/controller.h"
#include "esphome/core/log.h"
#include "esphome/core/application.h"
#include <StreamString.h>
#include <Updater.h>
#include <vector>
#include <FS.h>
#include "esphome/core/helpers.h"
#include <ESPAsyncUDP.h>

using namespace esphome;
using namespace std;
static const char *TAG = "BRIDGE";

#define serialSpeed        (2000000ul)
//#define serialSpeed        (115200ul)
/* Character timeout for end of frame detection */
#define characterBreakTime (5ul + 2*(10000000ul/serialSpeed))

#define BUFFER_SIZE  (8 * 1024)
#define RX_BUFFER_TH (1500)


class RingBuffer
{
public:
	RingBuffer(size_t capacity) {
		/* One byte is used for detecting the full condition. */
		m_size   = capacity + 1;
		m_buf    = new char[m_size];
		m_bufend = m_buf + m_size;
		if (m_buf) reset();
	}
	~RingBuffer()       { delete [] m_buf; }
	void reset()        { m_head = m_tail = m_buf; }
	size_t size()       { return m_size; }
	size_t capacity()   { return m_size - 1; }
	size_t bytes_free() { if (m_head >= m_tail) return (capacity() - (m_head - m_tail)); else return (m_tail - m_head - 1); }
	size_t bytes_used() { return capacity() - bytes_free(); }
	bool is_full()      { return bytes_free() == 0; }
	bool is_empty()     { return bytes_free() == capacity(); }

	/*!
	 * \brief Write data to ring buffer.
	 */
	size_t write(const char *src, size_t count) {
		size_t nread = 0;
		if (!m_buf) return 0;
		//if (count > bytes_free()) count = bytes_free();
		while (nread != count) {
			/* don't copy beyond the end of the buffer */
			size_t n = min((size_t)(m_bufend - m_head), (count - nread));
			os_memcpy(m_head, src, n);
			m_head += n;
			nread  += n;
			src    += n;
			/* wrap? */
			if (m_head == m_bufend) m_head = m_buf;
		}
		return nread;
	}

	/*!
	 * \brief Read data from ring buffer.
	 */
	size_t read(char *dst, size_t count) {
		size_t nwritten = 0;
		size_t bytesused = bytes_used();

		if (!m_buf) return 0;
		if (count > bytesused) count = bytesused;
		while (nwritten != count) {
			size_t n = min((size_t)(m_bufend - m_tail), (count - nwritten));
			os_memcpy(dst, m_tail, n);
			m_tail   += n;
			nwritten += n;
			dst      += n;
			/* wrap ? */
			if (m_tail == m_bufend) m_tail = m_buf;
		}
		return nwritten;
	}

	/*!
	 * \brief Fill buffer from serial port.
	 */
	size_t fillFromSerial() {
		size_t count, nread = 0;
		
		count = Serial.available();

		/* sanitary checks */
		if (!m_buf) return 0;
		if (count > bytes_free()) count = bytes_free();
		if (count == 0) return 0;

		while (nread != count) {
			size_t n = min((size_t)(m_bufend - m_head), (count - nread));
			Serial.read(m_head, n);
			m_head += n;
			nread  += n;
			/* wrap? */
			if (m_head == m_bufend) m_head = m_buf;
		}
		return nread;
	}

	/*!
	 * \brief Flush buffer to serial port.
	 */
	bool flushToSerial() {
		size_t count, bytesused, nwritten = 0;
		
		/* sanitary checks */
		if (!m_buf) return false;
		bytesused = bytes_used();
		count     = Serial.availableForWrite();
		if (count > bytesused) count = bytesused;
		if (count == 0) return false;
		/* Ok - flush data to serial */
		while (nwritten != count) {
			size_t n = min((size_t)(m_bufend - m_tail), (count - nwritten));
			Serial.write(m_tail, n);
			m_tail   += n;
			nwritten += n;
			/* wrap ? */
			if (m_tail == m_bufend) m_tail = m_buf;
		}
		return true;
	}

	bool flushToUDP(AsyncUDP *udp, IPAddress addr, uint16_t port) {
		size_t count, bytesused, nwritten = 0;
		char tmpBuf[RX_BUFFER_TH];
		char *pt = tmpBuf;
		/* sanitary checks */
		if (!m_buf)  return false;
		bytesused  = bytes_used();
		count      = RX_BUFFER_TH;
		if (count > bytesused) count = bytesused;
		if (count == 0) return false;
		/* Ok - flush data to TCP */
		while (nwritten != count) {
			size_t n = min((size_t)(m_bufend - m_tail), (count - nwritten));
			os_memcpy(pt, m_tail, n);
			m_tail   += n;
			nwritten += n;
			pt       += n;
			/* wrap ? */
			if (m_tail == m_bufend) m_tail = m_buf;
		}
		udp->writeTo((uint8_t *)tmpBuf,nwritten, addr, port);
		return true;
	}

private:
	size_t m_size;
	char  *m_buf;
	char  *m_head;
	char  *m_tail;
	char  *m_bufend;
};
//==========================================================================================


class BRIDGE;

class BRIDGE: public Component, public UARTDevice {
public:
	BRIDGE(UARTComponent *parent) : UARTDevice(parent) {}

	void init() {}
	//==========================================================================================
	
	void deinit() {}
	//==========================================================================================
	
	void setup() override {
		m_previousMicros = 0;          /*!< Previous micros for timeout computations      */
		m_lastBytes      = 0;          /*!< Last avail bytes for timeout computations     */
		m_tx_buffer      = new RingBuffer(BUFFER_SIZE);
		m_rx_buffer      = new RingBuffer(BUFFER_SIZE);
		if (m_udp.listen(1234)) {
			ESP_LOGD(TAG, "UDP Listening...");
			m_udp.onPacket([this](AsyncUDPPacket packet) {
				m_tx_buffer->write((const char *)packet.data(), packet.length());
				m_addr = packet.remoteIP();
				m_port = packet.remotePort();
			});
		}
		m_hf.start();
	}
	//==========================================================================================

	/*!
	 * \brief The main loop.
	 * 1) Send packets from TX queue over uart port.
	 * 2) Collect data from uart and push it to RX queue.
	 */
	void loop() override {
		uint64_t currentMicros;
		int avail;
		bool doTx = false;
		
		/* Send data from TX queue */
		m_tx_buffer->flushToSerial();

		/* Collect data and push it to RX queue */
		m_rx_buffer->fillFromSerial();
		currentMicros = micros64();
		avail = m_rx_buffer->bytes_used();
		if (avail) {
			if (avail < RX_BUFFER_TH) {
				if (avail > m_lastBytes) {
					m_previousMicros = currentMicros;
					m_lastBytes      = avail;
				} else if ((currentMicros - m_previousMicros) > characterBreakTime) {
					/* Transmision break timeout  */
					doTx = true;
				}
			} else {
				/* Buffer is full */
				doTx = true;
			}
		}
		if (doTx) {
			m_rx_buffer->flushToUDP(&m_udp, m_addr, m_port);
			m_previousMicros = currentMicros;
			m_lastBytes      = m_rx_buffer->bytes_used();
		}
	}
	//==========================================================================================
public:
	AsyncUDP         m_udp;
	RingBuffer      *m_tx_buffer;
	RingBuffer      *m_rx_buffer;
	IPAddress        m_addr;
	uint16_t         m_port;
	uint64_t         m_previousMicros;          /*!< Previous micros for timeout computations      */
	int              m_lastBytes;               /*!< Last avail bytes for timeout computations     */
	HighFrequencyLoopRequester m_hf;
};


