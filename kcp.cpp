//=====================================================================
//
// KCP - A Better ARQ Protocol Implementation
// Original author: skywind3000 (at) gmail.com, 2010-2011
// Modifier: cnbatch, 2021
//  
// Features:
// + Average RTT reduce 30% - 40% vs traditional ARQ like tcp.
// + Maximum RTT reduce three times vs tcp.
// + Lightweight, distributed as a single source file.
//
//=====================================================================
#include "kcp.hpp"

#include <cstddef>
#include <cstdlib>
#include <cstdint>
#include <cstdarg>
#include <cstdio>
#include <memory.h>


namespace kcp
{
	using internal_impl::segment;

	//=====================================================================
	// KCP BASIC
	//=====================================================================
	constexpr uint32_t IKCP_RTO_NDL = 30;		// no delay min rto
	constexpr uint32_t IKCP_RTO_MIN = 100;		// normal min rto
	constexpr uint32_t IKCP_RTO_DEF = 200;
	constexpr uint32_t IKCP_RTO_MAX = 60000;
	constexpr uint32_t IKCP_CMD_PUSH = 81;		// cmd: push data
	constexpr uint32_t IKCP_CMD_ACK = 82;		// cmd: ack
	constexpr uint32_t IKCP_CMD_WASK = 83;		// cmd: window probe (ask)
	constexpr uint32_t IKCP_CMD_WINS = 84;		// cmd: window size (tell)
	constexpr uint32_t IKCP_ASK_SEND = 1;		// need to send IKCP_CMD_WASK
	constexpr uint32_t IKCP_ASK_TELL = 2;		// need to send IKCP_CMD_WINS
	constexpr uint32_t IKCP_WND_SND = 32;
	constexpr uint32_t IKCP_WND_RCV = 128;       // must >= max fragment size
	constexpr uint32_t IKCP_MTU_DEF = 1400;
	constexpr uint32_t IKCP_ACK_FAST = 3;
	constexpr uint32_t IKCP_INTERVAL = 100;
	constexpr uint32_t IKCP_OVERHEAD = 24;
	constexpr uint32_t IKCP_DEADLINK = 20;
	constexpr uint32_t IKCP_THRESH_INIT = 2;
	constexpr uint32_t IKCP_THRESH_MIN = 2;
	constexpr uint32_t IKCP_PROBE_INIT = 7000;		// 7 secs to probe window size
	constexpr uint32_t IKCP_PROBE_LIMIT = 120000;	// up to 120 secs to probe window
	constexpr uint32_t IKCP_FASTACK_LIMIT = 5;		// max times to trigger fastack


	//---------------------------------------------------------------------
	// encode / decode
	//---------------------------------------------------------------------

	/* encode 8 bits unsigned int */
	inline char * kcp::encode8u(char *p, unsigned char c)
	{
		*(unsigned char*)p++ = c;
		return p;
	}

	/* decode 8 bits unsigned int */
	inline const char * kcp::decode8u(const char *p, unsigned char *c)
	{
		*c = *(unsigned char*)p++;
		return p;
	}

	/* encode 16 bits unsigned int (lsb) */
	inline char * kcp::encode16u(char *p, unsigned short w)
	{
#if IWORDS_BIG_ENDIAN || IWORDS_MUST_ALIGN
		*(unsigned char*)(p + 0) = (w & 255);
		*(unsigned char*)(p + 1) = (w >> 8);
#else
		memcpy(p, &w, 2);
#endif
		p += 2;
		return p;
	}

	/* decode 16 bits unsigned int (lsb) */
	inline const char * kcp::decode16u(const char *p, unsigned short *w)
	{
#if IWORDS_BIG_ENDIAN || IWORDS_MUST_ALIGN
		*w = *(const unsigned char*)(p + 1);
		*w = *(const unsigned char*)(p + 0) + (*w << 8);
#else
		memcpy(w, p, 2);
#endif
		p += 2;
		return p;
	}

	/* encode 32 bits unsigned int (lsb) */
	inline char * kcp::encode32u(char *p, uint32_t l)
	{
#if IWORDS_BIG_ENDIAN || IWORDS_MUST_ALIGN
		*(unsigned char*)(p + 0) = (unsigned char)((l >> 0) & 0xff);
		*(unsigned char*)(p + 1) = (unsigned char)((l >> 8) & 0xff);
		*(unsigned char*)(p + 2) = (unsigned char)((l >> 16) & 0xff);
		*(unsigned char*)(p + 3) = (unsigned char)((l >> 24) & 0xff);
#else
		memcpy(p, &l, 4);
#endif
		p += 4;
		return p;
	}

	/* decode 32 bits unsigned int (lsb) */
	inline const char * kcp::decode32u(const char *p, uint32_t *l)
	{
#if IWORDS_BIG_ENDIAN || IWORDS_MUST_ALIGN
		*l = *(const unsigned char*)(p + 3);
		*l = *(const unsigned char*)(p + 2) + (*l << 8);
		*l = *(const unsigned char*)(p + 1) + (*l << 8);
		*l = *(const unsigned char*)(p + 0) + (*l << 8);
#else 
		memcpy(l, p, 4);
#endif
		p += 4;
		return p;
	}

	//---------------------------------------------------------------------
	// Encode_seg
	//---------------------------------------------------------------------
	char * kcp::encode_seg(char *ptr, const segment &seg)
	{
		ptr = encode32u(ptr, seg.conv);
		ptr = encode8u(ptr, static_cast<uint8_t>(seg.cmd));
		ptr = encode8u(ptr, static_cast<uint8_t>(seg.frg));
		ptr = encode16u(ptr, static_cast<uint16_t>(seg.wnd));
		ptr = encode32u(ptr, seg.ts);
		ptr = encode32u(ptr, seg.sn);
		ptr = encode32u(ptr, seg.una);
		ptr = encode32u(ptr, static_cast<int>(seg.data.size()));
		return ptr;
	}

	static inline uint32_t _ibound_(uint32_t lower, uint32_t middle, uint32_t upper)
	{
		return std::min<uint32_t>(std::max<uint32_t>(lower, middle), upper);
	}

	static inline long _itimediff(uint32_t later, uint32_t earlier)
	{
		return static_cast<long>(later - earlier);
	}

	// write log
	void kcp::write_log(int mask, const char *fmt, ...)
	{
		char buffer[1024] = { 0 };
		va_list argptr;
		if ((mask & this->logmask) == 0 || this->writelog == nullptr) return;
		va_start(argptr, fmt);
		vsprintf(buffer, fmt, argptr);
		va_end(argptr);
		this->writelog(buffer, this->user);
	}

	// check log mask
	bool kcp::can_log(int mask)
	{
		return mask & this->logmask && this->writelog != nullptr;
	}

	// output segment
	int kcp::kcp_output(const void *data, int size)
	{
		assert(this->output);
		if (can_log(KCP_LOG_OUTPUT))
		{
			write_log(KCP_LOG_OUTPUT, "[RO] %ld bytes", static_cast<long>(size));
		}
		if (size == 0) return 0;
		return this->output((const char*)data, size, this->user);
	}

	// output queue
	void kcp::print_queue(const char *name, const std::list<segment> &segment_list)
	{
#if 0
		printf("<%s>: [", name);
		for (auto seg = segment_list.cbegin(), next = seg; seg != segment_list.cend(); seg = next)
		{
			++next;
			printf("(%lu %d)", (unsigned long)seg->sn, (int)(seg->ts % 10000));
			if (next != segment_list.cend()) printf(",");
		}
		printf("]\n");
#endif
	}


	//---------------------------------------------------------------------
	// create a new kcpcb
	//---------------------------------------------------------------------
	kcp::kcp(uint32_t conv, void *user)
	{
		this->conv = conv;
		this->user = user;
		this->snd_una = 0;
		this->snd_nxt = 0;
		this->rcv_nxt = 0;
		this->ts_recent = 0;
		this->ts_lastack = 0;
		this->ts_probe = 0;
		this->probe_wait = 0;
		this->snd_wnd = IKCP_WND_SND;
		this->rcv_wnd = IKCP_WND_RCV;
		this->rmt_wnd = IKCP_WND_RCV;
		this->cwnd = 0;
		this->incr = 0;
		this->probe = 0;
		this->mtu = IKCP_MTU_DEF;
		this->mss = this->mtu - IKCP_OVERHEAD;
		this->stream = 0;

		this->buffer.resize(static_cast<size_t>(this->mtu) + IKCP_OVERHEAD);

		this->state = 0;
		this->ackblock = 0;
		this->rx_srtt = 0;
		this->rx_rttval = 0;
		this->rx_rto = IKCP_RTO_DEF;
		this->rx_minrto = IKCP_RTO_MIN;
		this->current = 0;
		this->interval = IKCP_INTERVAL;
		this->ts_flush = IKCP_INTERVAL;
		this->nodelay = 0;
		this->updated = 0;
		this->logmask = 0;
		this->ssthresh = IKCP_THRESH_INIT;
		this->fastresend = 0;
		this->fastlimit = IKCP_FASTACK_LIMIT;
		this->nocwnd = 0;
		this->xmit = 0;
		this->dead_link = IKCP_DEADLINK;
	}


	//---------------------------------------------------------------------
	// release a new kcpcb
	//---------------------------------------------------------------------
	kcp::~kcp()
	{
	}


	//---------------------------------------------------------------------
	// set output callback, which will be invoked by kcp
	//---------------------------------------------------------------------
	void kcp::set_output(std::function<int(const char *, int, void *)> output)
	{
		this->output = output;
	}


	//---------------------------------------------------------------------
	// user/upper level recv: returns size, returns below zero for EAGAIN
	//---------------------------------------------------------------------
	int kcp::receive(char *buffer, int len)
	{
		bool ispeek = len < 0;
		bool recover = false;

		if (this->rcv_queue.empty())
			return -1;

		if (len < 0) len = -len;

		int peeksize = PeekSize();

		if (peeksize < 0)
			return -2;

		if (peeksize > len)
			return -3;

		if (this->rcv_queue.size() >= this->rcv_wnd)
			recover = true;

		// merge fragment
		len = 0;
		for (auto seg = rcv_queue.begin(), next = seg; seg != this->rcv_queue.end(); seg = next)
		{
			int fragment;
			++next;
			if (buffer)
			{
				std::copy(seg->data.begin(), seg->data.end(), buffer);
				//memcpy(buffer, seg->data.data(), seg->data.size());
				buffer += seg->data.size();
			}

			len += static_cast<int>(seg->data.size());
			fragment = seg->frg;

			if (can_log(KCP_LOG_RECV))
			{
				write_log(KCP_LOG_RECV, "recv sn=%lu", (unsigned long)seg->sn);
			}

			if (!ispeek)
			{
				seg = this->rcv_queue.erase(seg);
			}

			if (fragment == 0)
				break;
		}

		assert(len == peeksize);

		// move available data from rcv_buf -> rcv_queue
		while (!this->rcv_buf.empty())
		{
			auto seg = this->rcv_buf.begin();
			if (seg->sn == this->rcv_nxt && this->rcv_queue.size() < this->rcv_wnd)
			{
				this->rcv_queue.splice(this->rcv_queue.end(), this->rcv_buf, seg);
				this->rcv_nxt++;
			}
			else
			{
				break;
			}
		}

		// fast recover
		if (this->rcv_queue.size() < this->rcv_wnd && recover)
		{
			// ready to send back IKCP_CMD_WINS in Flush
			// tell remote my window size
			this->probe |= IKCP_ASK_TELL;
		}

		return len;
	}

	int kcp::receive(std::vector<char> &buffer)
	{
		int peeksize = PeekSize();

		if (peeksize < 0)
			return -2;

		if (peeksize > buffer.size())
			buffer.resize(peeksize);

		return receive(buffer.data(), static_cast<int>(buffer.size()));
	}

	//---------------------------------------------------------------------
	// peek data size
	//---------------------------------------------------------------------
	int kcp::PeekSize()
	{
		int length = 0;

		if (this->rcv_queue.empty()) return -1;

		auto seg = this->rcv_queue.begin();
		if (seg->frg == 0) return static_cast<int>(seg->data.size());

		if (this->rcv_queue.size() < static_cast<size_t>(seg->frg) + 1) return -1;

		for (seg = this->rcv_queue.begin(); seg != this->rcv_queue.end(); ++seg)
		{
			length += static_cast<int>(seg->data.size());
			if (seg->frg == 0) break;
		}

		return length;
	}


	//---------------------------------------------------------------------
	// user/upper level send, returns below zero for error
	//---------------------------------------------------------------------
	int kcp::send(const char *buffer, int len)
	{
		assert(this->mss > 0);
		if (len < 0) return -1;

		// append to previous segment in streaming mode (if possible)
		if (this->stream != 0)
		{
			if (!this->snd_queue.empty())
			{
				auto &seg = this->snd_queue.back();
				if (seg.data.size() < this->mss)
				{
					int capacity = this->mss - static_cast<int>(seg.data.size());
					int extend = (len < capacity) ? len : capacity;
					auto old_size = seg.data.size();
					seg.data.resize(seg.data.size() + extend);
					//memcpy(seg->data.data(), old->data.data(), old->data.size());
					if (buffer)
					{
						std::copy_n(buffer, extend, seg.data.begin() + old_size);
						//memcpy(seg.data.data() + old_size, buffer, extend);
						buffer += extend;
					}
					seg.frg = 0;
					len -= extend;
				}
			}
			if (len <= 0)
			{
				return 0;
			}
		}

		int count;

		if (len <= (int)this->mss) count = 1;
		else count = (len + this->mss - 1) / this->mss;

		if (count >= (int)IKCP_WND_RCV) return -2;

		if (count == 0) count = 1;

		// fragment
		for (int i = 0; i < count; i++)
		{
			int size = len > (int)this->mss ? (int)this->mss : len;
			this->snd_queue.emplace_back(segment(size));
			auto &seg = snd_queue.back();
			if (buffer && len > 0)
			{
				std::copy_n(buffer, size, seg.data.begin());
				//memcpy(seg.data.data(), buffer, size);
			}
			seg.frg = (this->stream == 0) ? (count - i - 1) : 0;
			if (buffer)
			{
				buffer += size;
			}
			len -= size;
		}

		return 0;
	}


	//---------------------------------------------------------------------
	// parse ack
	//---------------------------------------------------------------------
	void kcp::update_ack(int32_t rtt)
	{
		int32_t rto = 0;
		if (this->rx_srtt == 0)
		{
			this->rx_srtt = rtt;
			this->rx_rttval = rtt / 2;
		}
		else
		{
			long delta = rtt - this->rx_srtt;
			if (delta < 0) delta = -delta;
			this->rx_rttval = (3 * this->rx_rttval + delta) / 4;
			this->rx_srtt = (7 * this->rx_srtt + rtt) / 8;
			if (this->rx_srtt < 1) this->rx_srtt = 1;
		}
		rto = this->rx_srtt + std::max<uint32_t>(this->interval, 4 * this->rx_rttval);
		this->rx_rto = _ibound_(this->rx_minrto, rto, IKCP_RTO_MAX);
	}

	void kcp::shrink_buffer()
	{
		if (!this->snd_buf.empty())
		{
			this->snd_una = this->snd_buf.front().sn;
		}
		else
		{
			this->snd_una = this->snd_nxt;
		}
	}

	void kcp::parse_ack(uint32_t sn)
	{
		if (_itimediff(sn, this->snd_una) < 0 || _itimediff(sn, this->snd_nxt) >= 0)
			return;

		for (auto seg = this->snd_buf.begin(); seg != this->snd_buf.end(); ++seg)
		{
			if (sn == seg->sn)
			{
				this->snd_buf.erase(seg);
				break;
			}
			if (_itimediff(sn, seg->sn) < 0)
			{
				break;
			}
		}
	}

	void kcp::parse_una(uint32_t una)
	{
		for (auto seg = this->snd_buf.begin(); seg != this->snd_buf.end();)
		{
			if (_itimediff(una, seg->sn) > 0)
			{
				seg = this->snd_buf.erase(seg);
			}
			else
			{
				break;
			}
		}
	}

	void kcp::parse_fast_ack(uint32_t sn, uint32_t ts)
	{
		if (_itimediff(sn, this->snd_una) < 0 || _itimediff(sn, this->snd_nxt) >= 0)
			return;

		for (auto seg = this->snd_buf.begin(); seg != this->snd_buf.end(); ++seg)
		{
			if (_itimediff(sn, seg->sn) < 0)
			{
				break;
			}
			else if (sn != seg->sn)
			{
#ifndef IKCP_FASTACK_CONSERVE
				seg->fastack++;
#else
				if (_itimediff(ts, seg->ts) >= 0)
					seg->fastack++;
#endif
			}
		}
	}


	//---------------------------------------------------------------------
	// ack append
	//---------------------------------------------------------------------
	//void KCP::AckPush(uint32_t sn, uint32_t ts)
	//{
	//	this->acklist.push_back({ sn , ts });
	//}

	//void KCP::AckGet(int p, uint32_t *sn, uint32_t *ts)
	//{
	//	if (sn) sn[0] = this->acklist[p].first;
	//	if (ts) ts[0] = this->acklist[p].second;
	//}


	//---------------------------------------------------------------------
	// parse data
	//---------------------------------------------------------------------
	void kcp::parse_data(segment &newseg)
	{
		uint32_t sn = newseg.sn;
		bool repeat = false;

		if (_itimediff(sn, this->rcv_nxt + this->rcv_wnd) >= 0 ||
			_itimediff(sn, this->rcv_nxt) < 0)
		{
			return;
		}

		decltype(this->rcv_buf.rbegin()) seg_riter;
		for (seg_riter = this->rcv_buf.rbegin(); seg_riter != this->rcv_buf.rend(); ++seg_riter)
		{
			if (seg_riter->sn == sn)
			{
				repeat = true;
				break;
			}
			if (_itimediff(sn, seg_riter->sn) > 0)
			{
				break;
			}
		}

		if (!repeat)
		{
			this->rcv_buf.insert(seg_riter.base(), std::move(newseg));
		}

#if 0
		PrintQueue("rcvbuf", &this->rcv_buf);
		printf("rcv_nxt=%lu\n", this->rcv_nxt);
#endif

		// move available data from rcv_buf -> rcv_queue
		while (!this->rcv_buf.empty())
		{
			auto seg = this->rcv_buf.begin();
			if (seg->sn == this->rcv_nxt && this->rcv_queue.size() < this->rcv_wnd)
			{
				this->rcv_queue.splice(this->rcv_queue.end(), this->rcv_buf, seg);
				this->rcv_nxt++;
			}
			else
			{
				break;
			}
		}

#if 0
		PrintQueue("queue", &this->rcv_queue);
		printf("rcv_nxt=%lu\n", this->rcv_nxt);
#endif

#if 1
		//	printf("snd(buf=%d, queue=%d)\n", this->nsnd_buf, this->nsnd_que);
		//	printf("rcv(buf=%d, queue=%d)\n", this->nrcv_buf, this->nrcv_que);
#endif
	}


	//---------------------------------------------------------------------
	// input data
	//---------------------------------------------------------------------
	int kcp::input(const char *data, long size)
	{
		uint32_t prev_una = this->snd_una;
		uint32_t maxack = 0, latest_ts = 0;
		int flag = 0;

		if (can_log(KCP_LOG_INPUT))
		{
			write_log(KCP_LOG_INPUT, "[RI] %d bytes", (int)size);
		}

		if (data == NULL || (int)size < (int)IKCP_OVERHEAD) return -1;

		while (1)
		{
			uint32_t ts, sn, len, una, conv;
			uint16_t wnd;
			uint8_t cmd, frg;

			if (size < (int)IKCP_OVERHEAD) break;

			data = decode32u(data, &conv);
			if (conv != this->conv) return -1;

			data = decode8u(data, &cmd);
			data = decode8u(data, &frg);
			data = decode16u(data, &wnd);
			data = decode32u(data, &ts);
			data = decode32u(data, &sn);
			data = decode32u(data, &una);
			data = decode32u(data, &len);

			size -= IKCP_OVERHEAD;

			if ((long)size < (long)len || (int)len < 0) return -2;

			if (cmd != IKCP_CMD_PUSH && cmd != IKCP_CMD_ACK &&
				cmd != IKCP_CMD_WASK && cmd != IKCP_CMD_WINS)
				return -3;

			this->rmt_wnd = wnd;
			parse_una(una);
			shrink_buffer();

			if (cmd == IKCP_CMD_ACK)
			{
				if (_itimediff(this->current, ts) >= 0)
				{
					update_ack(_itimediff(this->current, ts));
				}
				parse_ack(sn);
				shrink_buffer();
				if (flag == 0)
				{
					flag = 1;
					maxack = sn;
					latest_ts = ts;
				}
				else
				{
					if (_itimediff(sn, maxack) > 0)
					{
#ifndef IKCP_FASTACK_CONSERVE
						maxack = sn;
						latest_ts = ts;
#else
						if (_itimediff(ts, latest_ts) > 0)
						{
							maxack = sn;
							latest_ts = ts;
						}
#endif
					}
				}
				if (can_log(KCP_LOG_IN_ACK))
				{
					write_log(KCP_LOG_IN_ACK,
						"input ack: sn=%lu rtt=%ld rto=%ld", (unsigned long)sn,
						(long)_itimediff(this->current, ts),
						(long)this->rx_rto);
				}
			}
			else if (cmd == IKCP_CMD_PUSH)
			{
				if (can_log(KCP_LOG_IN_DATA))
				{
					write_log(KCP_LOG_IN_DATA,
						"input psh: sn=%lu ts=%lu", (unsigned long)sn, (unsigned long)ts);
				}
				if (_itimediff(sn, this->rcv_nxt + this->rcv_wnd) < 0)
				{
					// ack append
					this->acklist.push_back({ sn , ts });
					if (_itimediff(sn, this->rcv_nxt) >= 0)
					{
						segment seg(len);
						seg.conv = conv;
						seg.cmd = cmd;
						seg.frg = frg;
						seg.wnd = wnd;
						seg.ts = ts;
						seg.sn = sn;
						seg.una = una;

						if (len > 0)
						{
							std::copy_n(data, len, seg.data.begin());
							//memcpy(seg.data.data(), data, len);
						}

						parse_data(seg);
					}
				}
			}
			else if (cmd == IKCP_CMD_WASK)
			{
				// ready to send back IKCP_CMD_WINS in Flush
				// tell remote my window size
				this->probe |= IKCP_ASK_TELL;
				if (can_log(KCP_LOG_IN_PROBE))
				{
					write_log(KCP_LOG_IN_PROBE, "input probe");
				}
			}
			else if (cmd == IKCP_CMD_WINS)
			{
				// do nothing
				if (can_log(KCP_LOG_IN_WINS))
				{
					write_log(KCP_LOG_IN_WINS,
						"input wins: %lu", (unsigned long)(wnd));
				}
			}
			else
			{
				return -3;
			}

			data += len;
			size -= len;
		}

		if (flag != 0)
		{
			parse_fast_ack(maxack, latest_ts);
		}

		if (_itimediff(this->snd_una, prev_una) > 0)
		{
			if (this->cwnd < this->rmt_wnd)
			{
				uint32_t mss = this->mss;
				if (this->cwnd < this->ssthresh)
				{
					this->cwnd++;
					this->incr += mss;
				}
				else
				{
					if (this->incr < mss) this->incr = mss;
					this->incr += (mss * mss) / this->incr + (mss / 16);
					if ((this->cwnd + 1) * mss <= this->incr)
					{
#if 1
						this->cwnd = (this->incr + mss - 1) / ((mss > 0) ? mss : 1);
#else
						this->cwnd++;
#endif
					}
				}
				if (this->cwnd > this->rmt_wnd)
				{
					this->cwnd = this->rmt_wnd;
					this->incr = this->rmt_wnd * mss;
				}
			}
		}

		return 0;
	}

	int kcp::window_unused()
	{
		if (this->rcv_queue.size() < this->rcv_wnd)
		{
			return this->rcv_wnd - static_cast<int>(this->rcv_queue.size());
		}
		return 0;
	}


	//---------------------------------------------------------------------
	// Flush
	//---------------------------------------------------------------------
	void kcp::flush()
	{
		uint32_t current = this->current;
		char *buffer = this->buffer.data();
		char *ptr = buffer;
		int size, i;
		uint32_t resent, cwnd;
		uint32_t rtomin;
		int change = 0;
		int lost = 0;

		// 'Update' haven't been called. 
		if (this->updated == 0) return;

		segment seg;
		seg.conv = this->conv;
		seg.cmd = IKCP_CMD_ACK;
		seg.frg = 0;
		seg.wnd = window_unused();
		seg.una = this->rcv_nxt;
		seg.sn = 0;
		seg.ts = 0;

		// flush acknowledges
		for (i = 0; i < this->acklist.size(); i++)
		{
			size = (int)(ptr - buffer);
			if (size + (int)IKCP_OVERHEAD > (int)this->mtu)
			{
				kcp_output(buffer, size);
				ptr = buffer;
			}
			seg.sn = this->acklist[i].first;
			seg.ts = this->acklist[i].second;
			ptr = encode_seg(ptr, seg);
		}

		this->acklist.clear();

		// probe window size (if remote window size equals zero)
		if (this->rmt_wnd == 0)
		{
			if (this->probe_wait == 0)
			{
				this->probe_wait = IKCP_PROBE_INIT;
				this->ts_probe = this->current + this->probe_wait;
			}
			else
			{
				if (_itimediff(this->current, this->ts_probe) >= 0)
				{
					if (this->probe_wait < IKCP_PROBE_INIT)
						this->probe_wait = IKCP_PROBE_INIT;
					this->probe_wait += this->probe_wait / 2;
					if (this->probe_wait > IKCP_PROBE_LIMIT)
						this->probe_wait = IKCP_PROBE_LIMIT;
					this->ts_probe = this->current + this->probe_wait;
					this->probe |= IKCP_ASK_SEND;
				}
			}
		}
		else
		{
			this->ts_probe = 0;
			this->probe_wait = 0;
		}

		// flush window probing commands
		if (this->probe & IKCP_ASK_SEND)
		{
			seg.cmd = IKCP_CMD_WASK;
			size = (int)(ptr - buffer);
			if (size + (int)IKCP_OVERHEAD > (int)this->mtu)
			{
				kcp_output(buffer, size);
				ptr = buffer;
			}
			ptr = encode_seg(ptr, seg);
		}

		// flush window probing commands
		if (this->probe & IKCP_ASK_TELL)
		{
			seg.cmd = IKCP_CMD_WINS;
			size = (int)(ptr - buffer);
			if (size + (int)IKCP_OVERHEAD > (int)this->mtu)
			{
				kcp_output(buffer, size);
				ptr = buffer;
			}
			ptr = encode_seg(ptr, seg);
		}

		this->probe = 0;

		// calculate window size
		cwnd = std::min<uint32_t>(this->snd_wnd, this->rmt_wnd);
		if (this->nocwnd == 0) cwnd = std::min<uint32_t>(this->cwnd, cwnd);

		// move data from snd_queue to snd_buf
		while (_itimediff(this->snd_nxt, this->snd_una + cwnd) < 0)
		{
			if (this->snd_queue.empty()) break;

			auto newseg = this->snd_queue.begin();

			this->snd_buf.splice(this->snd_buf.end(), this->snd_queue, newseg);

			newseg->conv = this->conv;
			newseg->cmd = IKCP_CMD_PUSH;
			newseg->wnd = seg.wnd;
			newseg->ts = current;
			newseg->sn = this->snd_nxt++;
			newseg->una = this->rcv_nxt;
			newseg->resendts = current;
			newseg->rto = this->rx_rto;
			newseg->fastack = 0;
			newseg->xmit = 0;
		}

		// calculate resent
		resent = (this->fastresend > 0) ? (uint32_t)this->fastresend : 0xffffffff;
		rtomin = (this->nodelay == 0) ? (this->rx_rto >> 3) : 0;

		// flush data segments
		for (auto seg_iter = this->snd_buf.begin(); seg_iter != this->snd_buf.end(); ++seg_iter)
		{
			bool needsend = false;
			if (seg_iter->xmit == 0)
			{
				needsend = true;
				seg_iter->xmit++;
				seg_iter->rto = this->rx_rto;
				seg_iter->resendts = current + seg_iter->rto + rtomin;
			}
			else if (_itimediff(current, seg_iter->resendts) >= 0)
			{
				needsend = true;
				seg_iter->xmit++;
				this->xmit++;
				if (this->nodelay == 0)
				{
					seg_iter->rto += std::max<uint32_t>(seg_iter->rto, static_cast<uint32_t>(this->rx_rto));
				}
				else
				{
					int32_t step = (this->nodelay < 2) ? static_cast<int32_t>(seg_iter->rto) : this->rx_rto;
					seg_iter->rto += step / 2;
				}
				seg_iter->resendts = current + seg_iter->rto;
				lost = 1;
			}
			else if (seg_iter->fastack >= resent)
			{
				if ((int)seg_iter->xmit <= this->fastlimit ||
					this->fastlimit <= 0)
				{
					needsend = true;
					seg_iter->xmit++;
					seg_iter->fastack = 0;
					seg_iter->resendts = current + seg_iter->rto;
					change++;
				}
			}

			if (needsend)
			{
				int need;
				seg_iter->ts = current;
				seg_iter->wnd = seg.wnd;
				seg_iter->una = this->rcv_nxt;

				size = (int)(ptr - buffer);
				need = IKCP_OVERHEAD + static_cast<int>(seg_iter->data.size());

				if (size + need > (int)this->mtu)
				{
					kcp_output(buffer, size);
					ptr = buffer;
				}

				ptr = encode_seg(ptr, *seg_iter);

				if (seg_iter->data.size() > 0)
				{
					std::copy(seg_iter->data.begin(), seg_iter->data.end(), ptr);
					//memcpy(ptr, segment->data.data(), segment->data.size());
					ptr += seg_iter->data.size();
				}

				if (seg_iter->xmit >= this->dead_link)
				{
					this->state = (uint32_t)-1;
				}
			}
		}

		// flash remain segments
		size = static_cast<int>(ptr - buffer);
		if (size > 0)
		{
			kcp_output(buffer, size);
		}

		// update ssthresh
		if (change)
		{
			uint32_t inflight = this->snd_nxt - this->snd_una;
			this->ssthresh = inflight / 2;
			if (this->ssthresh < IKCP_THRESH_MIN)
				this->ssthresh = IKCP_THRESH_MIN;
			this->cwnd = this->ssthresh + resent;
			this->incr = this->cwnd * this->mss;
		}

		if (lost)
		{
			this->ssthresh = cwnd / 2;
			if (this->ssthresh < IKCP_THRESH_MIN)
				this->ssthresh = IKCP_THRESH_MIN;
			this->cwnd = 1;
			this->incr = this->mss;
		}

		if (this->cwnd < 1)
		{
			this->cwnd = 1;
			this->incr = this->mss;
		}
	}


	//---------------------------------------------------------------------
	// update state (call it repeatedly, every 10ms-100ms), or you can ask 
	// check() when to call it again (without Input/Send calling).
	// 'current' - current timestamp in millisec. 
	//---------------------------------------------------------------------
	void kcp::update(uint32_t current)
	{
		this->current = current;

		if (this->updated == 0)
		{
			this->updated = 1;
			this->ts_flush = this->current;
		}

		int32_t slap = _itimediff(this->current, this->ts_flush);

		if (slap >= 10000 || slap < -10000)
		{
			this->ts_flush = this->current;
			slap = 0;
		}

		if (slap >= 0)
		{
			this->ts_flush += this->interval;
			if (_itimediff(this->current, this->ts_flush) >= 0)
			{
				this->ts_flush = this->current + this->interval;
			}
			flush();
		}
	}


	//---------------------------------------------------------------------
	// Determine when should you invoke Update:
	// returns when you should invoke Update in millisec, if there 
	// is no Input/Send calling. you can call Update in that
	// time, instead of call update repeatly.
	// Important to reduce unnacessary Update invoking. use it to 
	// schedule Update (eg. implementing an epoll-like mechanism, 
	// or optimize Update when handling massive kcp connections)
	//---------------------------------------------------------------------
	uint32_t kcp::check(uint32_t current)
	{
		uint32_t ts_flush = this->ts_flush;
		int32_t tm_flush = 0x7fffffff;
		int32_t tm_packet = 0x7fffffff;

		if (this->updated == 0)
		{
			return current;
		}

		if (_itimediff(current, ts_flush) >= 10000 ||
			_itimediff(current, ts_flush) < -10000)
		{
			ts_flush = current;
		}

		if (_itimediff(current, ts_flush) >= 0)
		{
			return current;
		}

		tm_flush = _itimediff(ts_flush, current);

		for (auto seg = this->snd_buf.cbegin(); seg != this->snd_buf.cend(); ++seg)
		{
			int32_t diff = _itimediff(seg->resendts, current);
			if (diff <= 0)
			{
				return current;
			}
			if (diff < tm_packet) tm_packet = diff;
		}

		uint32_t minimal = static_cast<uint32_t>(tm_packet < tm_flush ? tm_packet : tm_flush);
		if (minimal >= this->interval) minimal = this->interval;

		return current + minimal;
	}

	int kcp::set_mtu(int mtu)
	{
		if (mtu < 50 || mtu < (int)IKCP_OVERHEAD)
			return -1;
		if (this->mtu == mtu)
			return 0;
		this->mtu = mtu;
		this->mss = this->mtu - IKCP_OVERHEAD;
		this->buffer.resize(static_cast<size_t>(mtu) + IKCP_OVERHEAD);
		return 0;
	}
	
	int kcp::get_mtu()
	{
		return this->mtu;
	}

	int kcp::get_interval(int interval)
	{
		if (interval > 5000) interval = 5000;
		else if (interval < 10) interval = 10;
		this->interval = interval;
		return 0;
	}

	int kcp::no_delay(int nodelay, int interval, int resend, int nc)
	{
		if (nodelay >= 0)
		{
			this->nodelay = nodelay;
			if (nodelay)
			{
				this->rx_minrto = IKCP_RTO_NDL;
			}
			else
			{
				this->rx_minrto = IKCP_RTO_MIN;
			}
		}
		if (interval >= 0)
		{
			if (interval > 5000) interval = 5000;
			else if (interval < 10) interval = 10;
			this->interval = interval;
		}
		if (resend >= 0)
		{
			this->fastresend = resend;
		}
		if (nc >= 0)
		{
			this->nocwnd = nc;
		}
		return 0;
	}


	void kcp::set_window_size(int sndwnd, int rcvwnd)
	{
		if (sndwnd > 0)
		{
			this->snd_wnd = sndwnd;
		}
		if (rcvwnd > 0)
		{   // must >= max fragment size
			this->rcv_wnd = std::max<uint32_t>(rcvwnd, IKCP_WND_RCV);
		}
	}

	void kcp::get_window_size(int &sndwnd, int &rcvwnd)
	{
		sndwnd = this->snd_wnd;
		rcvwnd = this->rcv_wnd;
	}

	int kcp::waiting_for_send()
	{
		return static_cast<int>(this->snd_buf.size() + this->snd_queue.size());
	}

	// read conv
	uint32_t kcp::get_conv(const void *ptr)
	{
		uint32_t conv;
		decode32u(static_cast<const char*>(ptr), &conv);
		return conv;
	}

	uint32_t kcp::get_conv()
	{
		return this->conv;
	}

	void kcp::set_stream_mode(bool enable)
	{
		this->stream = enable;
	}

	int32_t& kcp::rx_min_rto()
	{
		return this->rx_minrto;
	}

	int& kcp::log_mask()
	{
		return this->logmask;
	}
}
