#ifndef TCP_REASSEMBLY_H
#define TCP_REASSEMBLY_H

#include <netinet/tcp.h>

#include "sniff.h"
#include "pcap_queue_block.h"

class TcpReassemblyDataItem {
public: 
	TcpReassemblyDataItem() {
		this->data = NULL;
		this->datalen = 0;
		this->time.tv_sec = 0;
		this->time.tv_usec = 0;
	}
	TcpReassemblyDataItem(u_char *data, u_int32_t datalen, timeval time) {
		this->data = data;
		this->datalen = datalen;
		this->time = time;
	}
	void setFrom(TcpReassemblyDataItem &dataItem) {
		this->data = dataItem.data;
		this->datalen = dataItem.datalen;
		this->time = dataItem.time;
	}
	void destroy() {
		if(this->data) {
			delete [] this->data;
			this->data = NULL;
		}
	}
public:
	u_char *data;
	u_int32_t datalen;
	timeval time;
};

class TcpReassemblyData {
public:
	TcpReassemblyData() {
		this->forceAppendExpectContinue = false;
	}
	~TcpReassemblyData();
	void addRequest(u_char *data, u_int32_t datalen, timeval time) {
		request.push_back(TcpReassemblyDataItem(data, datalen, time));
	}
	void addResponse(u_char *data, u_int32_t datalen, timeval time) {
		response.push_back(TcpReassemblyDataItem(data, datalen, time));
	}
	void addExpectContinue(u_char *data, u_int32_t datalen, timeval time) {
		expectContinue.push_back(TcpReassemblyDataItem(data, datalen, time));
	}
	void addExpectContinueResponse(u_char *data, u_int32_t datalen, timeval time) {
		expectContinueResponse.push_back(TcpReassemblyDataItem(data, datalen, time));
	}
	bool isFill();
public:
	vector<TcpReassemblyDataItem> request;
	vector<TcpReassemblyDataItem> response;
	vector<TcpReassemblyDataItem> expectContinue;
	vector<TcpReassemblyDataItem> expectContinueResponse;
	bool forceAppendExpectContinue;
};

class TcpReassemblyProcessData {
public:
	virtual void processData(u_int32_t ip_src, u_int32_t ip_dst,
				 u_int16_t port_src, u_int16_t port_dst,
				 TcpReassemblyData *data) = 0;
};

struct TcpReassemblyLink_id {
	TcpReassemblyLink_id(u_int32_t ip_src = 0, u_int32_t ip_dst = 0, 
			     u_int16_t port_src = 0, u_int16_t port_dst = 0) {
		this->ip_src = ip_src;
		this->ip_dst = ip_dst;
		this->port_src = port_src; 
		this->port_dst = port_dst;
	}
	void reverse() {
		u_int32_t tmp = this->ip_src;
		this->ip_src = this->ip_dst;
		this->ip_dst = tmp;
		tmp = this->port_src;
		this->port_src = this->port_dst;
		this->port_dst = tmp;
	}
	u_int32_t ip_src;
	u_int32_t ip_dst;
	u_int16_t port_src;
	u_int16_t port_dst;
	bool operator < (const TcpReassemblyLink_id& other) const {
		return((this->ip_src < other.ip_src) ? 1 : (this->ip_src > other.ip_src) ? 0 :
		       (this->ip_dst < other.ip_dst) ? 1 : (this->ip_dst > other.ip_dst) ? 0 :
		       (this->port_src < other.port_src) ? 1 : (this->port_src > other.port_src) ? 0 :
		       (this->port_dst < other.port_dst));
	}
};

class TcpReassemblyStream_packet {
public:
	enum eState {
		NA = 0,
		CHECK,
		FAIL
	};
	TcpReassemblyStream_packet() {
		state = NA;
		//locked_packet = false;
	}
private:
	void lock_packet() {
		if(this->block_store/* && !locked_packet*/) {
			//static int countLock;
			//cout << "COUNT LOCK: " << (++countLock) << endl;
			this->block_store->lock_packet(this->block_store_index);
			//locked_packet = true;
		}
	}
	void unlock_packet() {
		if(this->block_store/* && locked_packet*/) {
			//static int countUnlock;
			//cout << "COUNT UNLOCK: " << (++countUnlock) << endl;
			this->block_store->unlock_packet(this->block_store_index);
			//locked_packet = false;
		}
	}
	void cleanState() {
		this->state = NA;
	}
private:
	timeval time;
	tcphdr header_tcp;
	u_int32_t next_seq;
	u_char *data;
	u_int32_t datalen;
	u_int32_t datacaplen;
	pcap_block_store *block_store;
	int block_store_index;
	eState state;
	//bool locked_packet;
friend class TcpReassemblyStream_packet_var;
friend class TcpReassemblyStream;
friend class TcpReassemblyLink;
};

class TcpReassemblyStream_packet_var {
public:
	~TcpReassemblyStream_packet_var() {
		map<uint32_t, TcpReassemblyStream_packet>::iterator iter;
		for(iter = this->queue.begin(); iter != this->queue.end(); iter++) {
			iter->second.unlock_packet();
		}
	}
	void push(TcpReassemblyStream_packet packet);
	u_int32_t getNextSeqCheck() {
		map<uint32_t, TcpReassemblyStream_packet>::iterator iter;
		for(iter = this->queue.begin(); iter != this->queue.end(); iter++) {
			if(iter->second.datalen &&
			   (iter->second.state == TcpReassemblyStream_packet::NA ||
			    iter->second.state == TcpReassemblyStream_packet::CHECK)) {
				return(iter->second.next_seq);
			}
		}
		return(0);
	}
private:
	void unlockPackets() {
		map<uint32_t, TcpReassemblyStream_packet>::iterator iter;
		for(iter = this->queue.begin(); iter != this->queue.end(); iter++) {
			iter->second.unlock_packet();
		}
	}
	void cleanState() {
		map<uint32_t, TcpReassemblyStream_packet>::iterator iter;
		for(iter = this->queue.begin(); iter != this->queue.end(); iter++) {
			iter->second.cleanState();
		}
	}
private:
	map<uint32_t, TcpReassemblyStream_packet> queue;
friend class TcpReassemblyStream;
friend class TcpReassemblyLink;
};

class TcpReassemblyStream {
public:
	enum eDirection {
		DIRECTION_NA = 0,
		DIRECTION_TO_DEST,
		DIRECTION_TO_SOURCE
	};
	enum eType {
		TYPE_DATA,
		TYPE_SYN_SENT,
		TYPE_SYN_RECV,
		TYPE_FIN,
		TYPE_RST
	};
	enum eHttpType {
		HTTP_TYPE_NA = 0,
		HTTP_TYPE_POST,
		HTTP_TYPE_GET,
		HTTP_TYPE_HEAD,
		HTTP_TYPE_HTTP
	};
	TcpReassemblyStream(class TcpReassemblyLink *link) {
		direction = DIRECTION_TO_DEST;
		type = TYPE_DATA;
		ack = 0;
		first_seq = 0;
		last_seq = 0;
		min_seq = 0;
		max_next_seq = 0;
		is_ok = false;
		completed = false;
		completed_finally = false;
		exists_data = false;
		complete_data = NULL;
		http_type = HTTP_TYPE_NA;
		http_content_length = 0;
		http_ok = false;
		http_expect_continue = false;
		http_ok_expect_continue_post = false;
		http_ok_expect_continue_data = false;
		detect_ok_max_next_seq = 0;
		_ignore_expect_continue = false;
		_only_check_psh = false;
		_force_wait_for_next_psh = false;
		last_packet_at_from_header = 0;
		this->link = link;
	}
	~TcpReassemblyStream() {
		this->cleanCompleteData(true);
	}
	void push(TcpReassemblyStream_packet packet);
	int ok(bool crazySequence = false, bool enableSimpleCmpMaxNextSeq = false, u_int32_t maxNextSeq = 0, 
	       bool enableCheckCompleteContent = false, TcpReassemblyStream *prevHttpStream = NULL, bool enableDebug = false);
	bool ok2_ec(u_int32_t nextAck, bool enableDebug = false);
	u_char *complete(u_int32_t *datalen, timeval *time, bool check = false, bool unlockPackets = true);
	bool saveCompleteData(bool unlockPackets = true, bool check = false, TcpReassemblyStream *prevHttpStream = NULL);
	void cleanCompleteData(bool destroy = false);
	TcpReassemblyDataItem getCompleteData(bool clean = false);
	void unlockPackets();
	void printContent(int level  = 0);
	bool checkOkPost(TcpReassemblyStream *nextStream = NULL);
private:
	bool checkCompleteContent();
	bool checkContentIsHttpRequest();
	void cleanPacketsState() {
		map<uint32_t, TcpReassemblyStream_packet_var>::iterator iter;
		for(iter = this->queue.begin(); iter != this->queue.end(); iter++) {
			iter->second.cleanState();
		}
		this->ok_packets.clear();
	}
	u_int32_t getLastSeqFromNextStream();
	eDirection direction;
	eType type;
	u_int32_t ack;
	u_int32_t first_seq;
	u_int32_t last_seq;
	u_int32_t min_seq;
	u_int32_t max_next_seq;
	map<uint32_t, TcpReassemblyStream_packet_var> queue;
	deque<d_u_int32_t> ok_packets;
	bool is_ok;
	bool completed;
	bool completed_finally;
	bool exists_data;
	TcpReassemblyDataItem *complete_data;
	eHttpType http_type;
	u_int32_t http_content_length;
	bool http_ok;
	bool http_expect_continue;
	bool http_ok_expect_continue_post;
	bool http_ok_expect_continue_data;
	u_int32_t detect_ok_max_next_seq;
	bool _ignore_expect_continue;
	bool _only_check_psh;
	bool _force_wait_for_next_psh;
	u_int64_t last_packet_at_from_header;
	TcpReassemblyLink *link;
friend class TcpReassemblyLink;
};

class TcpReassemblyLink {
public:
	enum eState {
		STATE_NA = 0,
		STATE_SYN_SENT,
		STATE_SYN_RECV,
		STATE_SYN_OK,
		STATE_SYN_FORCE_OK,
		STATE_RESET,
		STATE_CLOSE,
		STATE_CLOSED,
		STATE_CRAZY
	};
	class streamIterator {
		public:
			streamIterator(TcpReassemblyLink *link) {
				this->link = link;
				this->init();
			}
			bool init();
			bool next();
			bool nextAckInDirection();
			bool nextAckInReverseDirection();
			bool nextSeqInDirection();
			void print();
			u_int32_t getMaxNextSeq();
		private:
			bool findSynSent();
			bool findFirstDataToDest();
		public:
			TcpReassemblyStream *stream;
			eState state;
		private:
			TcpReassemblyLink *link;
	};
	TcpReassemblyLink(class TcpReassembly *reassembly,
			  u_int32_t ip_src = 0, u_int32_t ip_dst = 0, 
			  u_int16_t port_src = 0, u_int16_t port_dst = 0) {
		this->reassembly = reassembly;
		this->ip_src = ip_src;
		this->ip_dst = ip_dst;
		this->port_src = port_src; 
		this->port_dst = port_dst;
		this->state = STATE_NA;
		this->first_seq_to_dest = 0;
		this->first_seq_to_source = 0;
		this->rst = false;
		this->fin_to_dest = false;
		this->fin_to_source = false;
		this->_sync_queue = 0;
		this->created_at = getTimeMS();
		this->last_packet_at = 0;
		this->created_at_from_header = 0;
		this->last_packet_at_from_header = 0;
		this->last_ack = 0;
		this->exists_data = false;
		this->link_is_ok = 0;
		this->completed_offset = 0;
		this->direction_confirm = 0;
	}
	~TcpReassemblyLink();
	bool push(TcpReassemblyStream::eDirection direction,
		  timeval time, tcphdr header_tcp, 
		  u_char *data, u_int32_t datalen, u_int32_t datacaplen,
		  pcap_block_store *block_store, int block_store_index) {
		if(datalen) {
			this->exists_data = true;
		}
		if(this->state == STATE_CRAZY) {
			return(this->push_crazy(
				direction, time, header_tcp, 
				data, datalen, datacaplen,
				block_store, block_store_index));
		} else {
			return(this->push_normal(
				direction, time, header_tcp, 
				data, datalen, datacaplen,
				block_store, block_store_index));
		}
	}
	bool push_normal(
		  TcpReassemblyStream::eDirection direction,
		  timeval time, tcphdr header_tcp, 
		  u_char *data, u_int32_t datalen, u_int32_t datacaplen,
		  pcap_block_store *block_store, int block_store_index);
	bool push_crazy(
		  TcpReassemblyStream::eDirection direction,
		  timeval time, tcphdr header_tcp, 
		  u_char *data, u_int32_t datalen, u_int32_t datacaplen,
		  pcap_block_store *block_store, int block_store_index);
	int okQueue(bool final = false, bool enableDebug = false) {
		if(this->state == STATE_CRAZY) {
			return(this->okQueue_crazy(final, enableDebug));
		} else {
			/*
			return(this->okQueue_normal(final, enableDebug));
			*/
		}
		return(0);
	}
	int okQueue_normal(bool final = false, bool enableDebug = false);
	int okQueue_crazy(bool final = false, bool enableDebug = false);
	void complete(bool final = false, bool eraseCompletedStreams = false) {
		if(this->state == STATE_CRAZY) {
			this->complete_crazy(final, eraseCompletedStreams);
		} else {
			/*
			this->complete_normal();
			*/
		}
	}
	void complete_normal();
	void complete_crazy(bool final = false, bool eraseCompletedStreams = false);
	streamIterator createIterator();
	TcpReassemblyStream *findStreamBySeq(u_int32_t seq) {
		for(size_t i = 0; i < this->queue.size(); i++) {
			map<uint32_t, TcpReassemblyStream_packet_var>::iterator iter;
			iter = this->queue[i]->queue.find(seq);
			if(iter != this->queue[i]->queue.end()) {
				return(this->queue[i]);
			}
		}
		return(NULL);
	}
	TcpReassemblyStream *findStreamByMinSeq(u_int32_t seq, bool dataOnly = false, 
						u_int32_t not_ack = 0, TcpReassemblyStream::eDirection direction = TcpReassemblyStream::DIRECTION_NA) {
		map<uint32_t, TcpReassemblyStream*>::iterator iter;
		for(iter = this->queue_by_ack.begin(); iter != this->queue_by_ack.end(); iter++) {
			if(iter->second->min_seq == seq &&
			   (!not_ack || iter->second->ack != not_ack) &&
			   (direction == TcpReassemblyStream::DIRECTION_NA || iter->second->direction == direction)) {
				return(iter->second);
			}
		}
		if(!dataOnly && this->queue_nul_by_ack.size()) {
			iter = this->queue_nul_by_ack.end();
			do {
				--iter;
				if(iter->second->min_seq == seq &&
				   (!not_ack || iter->second->ack != not_ack) &&
				   (direction == TcpReassemblyStream::DIRECTION_NA || iter->second->direction == direction)) {
					return(iter->second);
				}
			} while(iter != this->queue_nul_by_ack.begin());
		}
		return(NULL);
	}
	TcpReassemblyStream *findFlagStreamByAck(u_int32_t ack) {
		map<uint32_t, TcpReassemblyStream*>::iterator iter;
		iter = this->queue_flags_by_ack.find(ack);
		if(iter != this->queue_flags_by_ack.end()) {
			return(iter->second);
		}
		return(NULL);
	}
	TcpReassemblyStream *findFinalFlagStreamByAck(u_int32_t ack, TcpReassemblyStream::eDirection direction) {
		map<uint32_t, TcpReassemblyStream*>::iterator iter;
		iter = this->queue_flags_by_ack.find(ack);
		if(iter != this->queue_flags_by_ack.end() &&
		   iter->second->direction == direction) {
			return(iter->second);
		}
		return(NULL);
	}
	TcpReassemblyStream *findFinalFlagStreamBySeq(u_int32_t seq, TcpReassemblyStream::eDirection direction) {
		map<uint32_t, TcpReassemblyStream*>::iterator iter;
		for(iter = this->queue_flags_by_ack.begin(); iter != this->queue_flags_by_ack.end(); iter++) {
			if(iter->second->direction == direction &&
			   iter->second->min_seq >= seq) {
				return(iter->second);
			}
		}
		return(NULL);
	}
	bool existsUncompletedDataStream() {
		map<uint32_t, TcpReassemblyStream*>::iterator iter;
		for(iter = this->queue_by_ack.begin(); iter != this->queue_by_ack.end(); iter++) {
			if(iter->second->exists_data &&
			   !iter->second->completed) {
				return(true);
			}
		}
		return(false);
	}
	void cleanup(u_int64_t act_time_from_header);
	void printContent(int level  = 0);
private:
	void lock_queue() {
		while(__sync_lock_test_and_set(&this->_sync_queue, 1));
	}
	void unlock_queue() {
		__sync_lock_release(&this->_sync_queue);
	}
	void pushpacket(TcpReassemblyStream::eDirection direction,
		        TcpReassemblyStream_packet packet);
	void setLastSeq(TcpReassemblyStream::eDirection direction, 
			u_int32_t lastSeq);
	void switchDirection();
private:
	TcpReassembly *reassembly;
	u_int32_t ip_src;
	u_int32_t ip_dst;
	u_int16_t port_src;
	u_int16_t port_dst;
	eState state;
	u_int32_t first_seq_to_dest;
	u_int32_t first_seq_to_source;
	bool rst;
	bool fin_to_dest;
	bool fin_to_source;
	map<uint32_t, TcpReassemblyStream*> queue_by_ack;
	map<uint32_t, TcpReassemblyStream*> queue_flags_by_ack;
	map<uint32_t, TcpReassemblyStream*> queue_nul_by_ack;
	deque<TcpReassemblyStream*> queue;
	volatile int _sync_queue;
	u_int64_t created_at;
	u_int64_t last_packet_at;
	u_int64_t created_at_from_header;
	u_int64_t last_packet_at_from_header;
	u_int32_t last_ack;
	bool exists_data;
	int link_is_ok;
	size_t completed_offset;
	int direction_confirm;
	vector<TcpReassemblyStream*> ok_streams;
friend class TcpReassembly;
friend class TcpReassemblyStream;
};

class TcpReassembly {
public:
	TcpReassembly() {
		this->_sync_links = 0;
		this->enableHttpForceInit = false;
		this->enableCrazySequence = false;
		this->dataCallback = NULL;
		this->act_time_from_header = 0;
		this->last_time = 0;
		this->doPrintContent = false;
	}
	~TcpReassembly();
	void push(pcap_pkthdr *header, iphdr2 *header_ip, u_char *packet,
		  pcap_block_store *block_store = NULL, int block_store_index = 0);
	void cleanup(bool all = false);
	void setEnableHttpForceInit(bool enableHttpForceInit = true) {
		this->enableHttpForceInit = enableHttpForceInit;
	}
	void setEnableCrazySequence(bool enableCrazySequence = true) {
		this->enableCrazySequence = enableCrazySequence;
	}
	void setDataCallback(TcpReassemblyProcessData *dataCallback) {
		this->dataCallback = dataCallback;
	}
	/*
	bool enableStop();
	*/
	void printContent();
	void setDoPrintContent() {
		this->doPrintContent = true;
	}
private:
	void lock_links() {
		while(__sync_lock_test_and_set(&this->_sync_links, 1));
	}
	void unlock_links() {
		__sync_lock_release(&this->_sync_links);
	}
private:
	map<TcpReassemblyLink_id, TcpReassemblyLink*> links;
	volatile int _sync_links;
	bool enableHttpForceInit;
	bool enableCrazySequence;
	TcpReassemblyProcessData *dataCallback;
	u_int64_t act_time_from_header;
	u_int64_t last_time;
	bool doPrintContent;
friend class TcpReassemblyLink;
};

#endif