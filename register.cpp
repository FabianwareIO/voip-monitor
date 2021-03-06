#include "voipmonitor.h"
#include "register.h"
#include "sql_db.h"
#include "record_array.h"
#include "fraud.h"


#define NEW_REGISTER_CLEAN_PERIOD 30
#define NEW_REGISTER_UPDATE_FAILED_PERIOD 20
#define NEW_REGISTER_NEW_RECORD_FAILED 120
#define NEW_REGISTER_ERASE_FAILED_TIMEOUT 60
#define NEW_REGISTER_ERASE_TIMEOUT 2*3600
#define NEW_REGISTER_USE_CMP_UA_STATE false


extern char sql_cdr_ua_table[256];
extern int opt_mysqlstore_max_threads_register;
extern MySqlStore *sqlStore;
extern int opt_nocdr;
extern int opt_enable_fraud;

Registers registers;


struct RegisterFields {
	eRegisterField filedType;
	const char *fieldName;
} registerFields[] = {
	{ rf_id, "ID" },
	{ rf_id_sensor, "id_sensor" },
	{ rf_fname, "fname" },
	{ rf_calldate, "calldate" },
	{ rf_sipcallerip, "sipcallerip" },
	{ rf_sipcalledip, "sipcalledip" },
	{ rf_from_num, "from_num" },
	{ rf_from_name, "from_name" },
	{ rf_from_domain, "from_domain" },
	{ rf_to_num, "to_num" },
	{ rf_to_domain, "to_domain" },
	{ rf_contact_num, "contact_num" },
	{ rf_contact_domain, "contact_domain" },
	{ rf_digestusername, "digestusername" },
	{ rf_digestrealm, "digestrealm" },
	{ rf_expires, "expires" },
	{ rf_expires_at, "expires_at" },
	{ rf_state, "state" },
	{ rf_ua, "ua" },
	{ rf_rrd_avg, "rrd_avg" }
};

SqlDb *sqlDbSaveRegister = NULL;


RegisterId::RegisterId(Register *reg) {
	this->reg = reg;
}

bool RegisterId:: operator == (const RegisterId& other) const {
	return(this->reg->sipcallerip == other.reg->sipcallerip &&
	       this->reg->sipcalledip == other.reg->sipcalledip &&
	       REG_EQ_STR(this->reg->to_num, other.reg->to_num) &&
	       REG_EQ_STR(this->reg->to_domain, other.reg->to_domain) &&
	       //REG_EQ_STR(this->reg->contact_num, other.reg->contact_num) &&
	       //REG_EQ_STR(this->reg->contact_domain, other.reg->contact_domain) &&
	       REG_EQ_STR(this->reg->digest_username, other.reg->digest_username));
}

bool RegisterId:: operator < (const RegisterId& other) const { 
	int rslt_cmp_to_num;
	int rslt_cmp_to_domain;
	//int rslt_cmp_contact_num;
	//int rslt_cmp_contact_domain;
	int rslt_cmp_digest_username;
	return((this->reg->sipcallerip < other.reg->sipcallerip) ? 1 : (this->reg->sipcallerip > other.reg->sipcallerip) ? 0 :
	       (this->reg->sipcalledip < other.reg->sipcalledip) ? 1 : (this->reg->sipcalledip > other.reg->sipcalledip) ? 0 :
	       ((rslt_cmp_to_num = REG_CMP_STR(this->reg->to_num, other.reg->to_num)) < 0) ? 1 : (rslt_cmp_to_num > 0) ? 0 :
	       ((rslt_cmp_to_domain = REG_CMP_STR(this->reg->to_domain, other.reg->to_domain)) < 0) ? 1 : (rslt_cmp_to_domain > 0) ? 0 :
	       //((rslt_cmp_contact_num = REG_CMP_STR(this->reg->contact_num, other.reg->contact_num)) < 0) ? 1 : (rslt_cmp_contact_num > 0) ? 0 :
	       //((rslt_cmp_contact_domain = REG_CMP_STR(this->reg->contact_domain, other.reg->contact_domain)) < 0) ? 1 : (rslt_cmp_contact_domain > 0) ? 0 :
	       ((rslt_cmp_digest_username = REG_CMP_STR(this->reg->digest_username, other.reg->digest_username)) < 0));
}


RegisterState::RegisterState(Call *call, Register *reg) {
	if(call) {
		char *tmp_str;
		state_from = state_to = call->calltime();
		counter = 1;
		state = convRegisterState(call);
		contact_num = reg->contact_num && REG_EQ_STR(call->contact_num, reg->contact_num) ?
			       EQ_REG :
			       REG_NEW_STR(call->contact_num);
		contact_domain = reg->contact_domain && REG_EQ_STR(call->contact_domain, reg->contact_domain) ?
				  EQ_REG :
				  REG_NEW_STR(call->contact_domain);
		from_num = reg->from_num && REG_EQ_STR(call->caller, reg->from_num) ?
			    EQ_REG :
			    REG_NEW_STR(call->caller);
		from_name = reg->from_name && REG_EQ_STR(call->callername, reg->from_name) ?
			     EQ_REG :
			     REG_NEW_STR(call->callername);
		from_domain = reg->from_domain && REG_EQ_STR(call->caller_domain, reg->from_domain) ?
			       EQ_REG :
			       REG_NEW_STR(call->caller_domain);
		digest_realm = reg->digest_realm && REG_EQ_STR(call->digest_realm, reg->digest_realm) ?
				EQ_REG :
				REG_NEW_STR(call->digest_realm);
		ua = reg->ua && REG_EQ_STR(call->a_ua, reg->ua) ?
		      EQ_REG :
		      REG_NEW_STR(call->a_ua);
		fname = call->fname_register;
		expires = call->register_expires;
		id_sensor = call->useSensorId;
	} else {
		state_from = state_to = 0;
		counter = 0;
		state = rs_na;
		contact_num = NULL;
		contact_domain = NULL;
		from_num = NULL;
		from_name = NULL;
		from_domain = NULL;
		digest_realm = NULL;
		ua = NULL;
	}
	db_id = 0;
	save_at = 0;
	save_at_counter = 0;
}

RegisterState::~RegisterState() {
	REG_FREE_STR(contact_num);
	REG_FREE_STR(contact_domain);
	REG_FREE_STR(from_num);
	REG_FREE_STR(from_name);
	REG_FREE_STR(from_domain);
	REG_FREE_STR(digest_realm);
	REG_FREE_STR(ua);
}

void RegisterState::copyFrom(const RegisterState *src) {
	*this = *src;
	char *tmp_str;
	contact_num = REG_NEW_STR(src->contact_num);
	contact_domain = REG_NEW_STR(src->contact_domain);
	from_num = REG_NEW_STR(src->from_num);
	from_name = REG_NEW_STR(src->from_name);
	from_domain = REG_NEW_STR(src->from_domain);
	digest_realm = REG_NEW_STR(src->digest_realm);
	ua = REG_NEW_STR(src->ua);
}

bool RegisterState::isEq(Call *call, Register *reg, bool useCmpUa) {
	/*
	if(state == convRegisterState(call)) cout << "ok state" << endl;
	//if(REG_EQ_STR(contact_num == EQ_REG ? reg->contact_num : contact_num, call->contact_num)) cout << "ok contact_num" << endl;
	//if(REG_EQ_STR(contact_domain == EQ_REG ? reg->contact_domain : contact_domain, call->contact_domain)) cout << "ok contact_domain" << endl;
	if(REG_EQ_STR(from_num == EQ_REG ? reg->from_num : from_num, call->caller)) cout << "ok from_num" << endl;
	if(REG_EQ_STR(from_name == EQ_REG ? reg->from_name : from_name, call->callername)) cout << "ok from_name" << endl;
	if(REG_EQ_STR(from_domain == EQ_REG ? reg->from_domain : from_domain, call->caller_domain)) cout << "ok from_domain" << endl;
	if(REG_EQ_STR(digest_realm == EQ_REG ? reg->digest_realm : digest_realm, call->digest_realm)) cout << "ok digest_realm" << endl;
	if(REG_EQ_STR(ua == EQ_REG ? reg->ua : ua, call->a_ua)) cout << "ok ua" << endl;
	*/
	return(state == convRegisterState(call) &&
	       //REG_EQ_STR(contact_num == EQ_REG ? reg->contact_num : contact_num, call->contact_num) &&
	       //REG_EQ_STR(contact_domain == EQ_REG ? reg->contact_domain : contact_domain, call->contact_domain) &&
	       REG_EQ_STR(from_num == EQ_REG ? reg->from_num : from_num, call->caller) &&
	       REG_EQ_STR(from_name == EQ_REG ? reg->from_name : from_name, call->callername) &&
	       REG_EQ_STR(from_domain == EQ_REG ? reg->from_domain : from_domain, call->caller_domain) &&
	       REG_EQ_STR(digest_realm == EQ_REG ? reg->digest_realm : digest_realm, call->digest_realm) &&
	       (!useCmpUa || REG_EQ_STR(ua == EQ_REG ? reg->ua : ua, call->a_ua)) &&
	       id_sensor == call->useSensorId);
}


Register::Register(Call *call) {
	lock_id();
	id = ++_id;
	unlock_id();
	sipcallerip = call->sipcallerip[0];
	sipcalledip = call->sipcalledip[0];
	char *tmp_str;
	to_num = REG_NEW_STR(call->called);
	to_domain = REG_NEW_STR(call->called_domain);
	contact_num = REG_NEW_STR(call->contact_num);
	contact_domain = REG_NEW_STR(call->contact_domain);
	digest_username = REG_NEW_STR(call->digest_username);
	from_num = REG_NEW_STR(call->caller);
	from_name = REG_NEW_STR(call->callername);
	from_domain = REG_NEW_STR(call->caller_domain);
	digest_realm = REG_NEW_STR(call->digest_realm);
	ua = REG_NEW_STR(call->a_ua);
	for(unsigned i = 0; i < NEW_REGISTER_MAX_STATES; i++) {
		states[i] = 0;
	}
	countStates = 0;
	rrd_sum = 0;
	rrd_count = 0;
	_sync_states = 0;
}

Register::~Register() {
	REG_FREE_STR(to_num);
	REG_FREE_STR(to_domain);
	REG_FREE_STR(contact_num);
	REG_FREE_STR(contact_domain);
	REG_FREE_STR(digest_username);
	REG_FREE_STR(from_num);
	REG_FREE_STR(from_name);
	REG_FREE_STR(from_domain);
	REG_FREE_STR(digest_realm);
	REG_FREE_STR(ua);
	clean_all();
}

void Register::update(Call *call) {
	char *tmp_str;
	if(!contact_num && call->contact_num[0]) {
		contact_num = REG_NEW_STR(call->contact_num);
	}
	if(!contact_domain && call->contact_domain[0]) {
		contact_domain = REG_NEW_STR(call->contact_domain);
	}
}

void Register::addState(Call *call) {
	lock_states();
	bool updateRsOk = false;
	bool updateRsFailedOk = false;
	if(convRegisterState(call) != rs_Failed) {
		if(eqLastState(call, NEW_REGISTER_USE_CMP_UA_STATE)) {
			updateLastState(call);
			updateRsOk = true;
		} else if(countStates > 1) {
			for(unsigned i = 1; i < countStates; i++) {
				if(states[i]->state != rs_Failed) {
					if(states[i]->isEq(call, this, NEW_REGISTER_USE_CMP_UA_STATE)) {
						RegisterState *state = states[i];
						for(unsigned j = i; j > 0; j--) {
							states[j] = states[j - 1];
						}
						states[0] = state;
						updateLastState(call);
						updateRsOk = true;
					}
					break;
				}
			}
		}
	} else {
		if(eqLastState(call, NEW_REGISTER_USE_CMP_UA_STATE)) {
			updateLastState(call);
			updateRsFailedOk = true;
		} else if(countStates > 1) {
			for(unsigned i = 1; i < countStates; i++) {
				if(states[i]->state == rs_Failed) {
					if(states[i]->isEq(call, this, NEW_REGISTER_USE_CMP_UA_STATE)) {
						RegisterState *failedState = states[i];
						for(unsigned j = i; j > 0; j--) {
							states[j] = states[j - 1];
						}
						states[0] = failedState;
						updateLastState(call);
						updateRsFailedOk = true;
					}
					break;
				}
			}
		}
	}
	if(!updateRsOk && !updateRsFailedOk) {
		shiftStates();
		states[0] = new FILE_LINE(20002) RegisterState(call, this);
		++countStates;
	}
	RegisterState *state = states_last();
	if((state->state == rs_OK || state->state == rs_UnknownMessageOK) &&
	   call->regrrddiff > 0) {
		rrd_sum += call->regrrddiff;
		++rrd_count;
	}
	if(state->state == rs_Failed) {
		saveFailedToDb(state);
	} else if(!updateRsOk) {
		saveStateToDb(state);
		RegisterState *prevState = states_prev_last();
		if(prevState && prevState->state == rs_Failed) {
			saveFailedToDb(prevState, true);
		}
	}
	if(opt_enable_fraud && isFraudReady()) {
		RegisterState *prevState = states_prev_last();
		fraudRegister(call, state->state, prevState ? prevState->state : rs_na, prevState ? prevState->state_to : 0);
	}
	unlock_states();
}

void Register::shiftStates() {
	if(countStates == NEW_REGISTER_MAX_STATES) {
		delete states[NEW_REGISTER_MAX_STATES - 1];
		-- countStates;
	}
	for(unsigned i = countStates; i > 0; i--) {
		states[i] = states[i - 1];
	}
}

void Register::expire(bool need_lock_states) {
	if(need_lock_states) {
		lock_states();
	}
	RegisterState *lastState = states_last();
	if(lastState && (lastState->state == rs_OK || lastState->state == rs_UnknownMessageOK)) {
		shiftStates();
		RegisterState *newState = new FILE_LINE(20003) RegisterState(NULL, NULL);
		newState->copyFrom(lastState);
		newState->state = rs_Expired;
		newState->expires = 0;
		newState->state_from = newState->state_to = lastState->state_to + lastState->expires;
		states[0] = newState;
		++countStates;
		saveStateToDb(newState);
		if(opt_enable_fraud && isFraudReady()) {
			RegisterState *prevState = states_prev_last();
			fraudRegister(this, prevState, rs_Expired, prevState ? prevState->state : rs_na, prevState ? prevState->state_to : 0);
		}
	}
	if(need_lock_states) {
		unlock_states();
	}
}

void Register::updateLastState(Call *call) {
	RegisterState *state = states_last();
	if(state) {
		state->state_to = call->calltime();
		++state->counter;
	}
}

bool Register::eqLastState(Call *call, bool useCmpUa) { 
	RegisterState *state = states_last();
	if(state && state->isEq(call, this, useCmpUa)) {
		return(true);
	}
	return(false);
}

void Register::clean_all() {
	lock_states();
	for(unsigned i = 0; i < countStates; i++) {
		delete states[i];
	}
	countStates = 0;
	unlock_states();
}

void Register::saveStateToDb(RegisterState *state, bool enableBatchIfPossible) {
	if(opt_nocdr) {
		return;
	}
	if(state->state == rs_ManyRegMessages) {
		return;
	}
	if(!sqlDbSaveRegister) {
		sqlDbSaveRegister = createSqlObject();
		sqlDbSaveRegister->setEnableSqlStringInContent(true);
	}
	string adj_ua = REG_CONV_STR(state->ua == EQ_REG ? ua : state->ua);
	adjustUA((char*)adj_ua.c_str());
	SqlDb_row cdr_ua;
	if(adj_ua[0]) {
		cdr_ua.add(sqlEscapeString(adj_ua), "ua");
	}
	SqlDb_row reg;
	reg.add(sqlEscapeString(sqlDateTimeString(state->state_from).c_str()), "created_at");
	reg.add(htonl(sipcallerip), "sipcallerip");
	reg.add(htonl(sipcalledip), "sipcalledip");
	reg.add(sqlEscapeString(REG_CONV_STR(state->from_num == EQ_REG ? from_num : state->from_num)), "from_num");
	reg.add(sqlEscapeString(REG_CONV_STR(to_num)), "to_num");
	reg.add(sqlEscapeString(REG_CONV_STR(state->contact_num == EQ_REG ? contact_num : state->contact_num)), "contact_num");
	reg.add(sqlEscapeString(REG_CONV_STR(state->contact_domain == EQ_REG ? contact_domain : state->contact_domain)), "contact_domain");
	reg.add(sqlEscapeString(REG_CONV_STR(to_domain)), "to_domain");
	reg.add(sqlEscapeString(REG_CONV_STR(digest_username)), "digestusername");
	reg.add(state->fname, "fname");
	if(state->state == rs_Failed) {
		reg.add(state->counter, "counter");
		state->db_id = registers.getNewRegisterFailedId(state->id_sensor);
		reg.add(state->db_id, "ID");
	} else {
		reg.add(state->expires, "expires");
		reg.add(state->state <= rs_Expired ? state->state : rs_OK, "state");
	}
	if(state->id_sensor > -1) {
		reg.add(state->id_sensor, "id_sensor");
	}
	string register_table = state->state == rs_Failed ? "register_failed" : "register_state";
	if(enableBatchIfPossible && isSqlDriver("mysql")) {
		string query_str;
		if(adj_ua[0]) {
			query_str += string("set @ua_id = ") +  "getIdOrInsertUA(" + sqlEscapeStringBorder(adj_ua) + ");\n";
			reg.add("_\\_'SQL'_\\_:@ua_id", "ua_id");
		}
		query_str += sqlDbSaveRegister->insertQuery(register_table, reg, false, false, state->state == rs_Failed) + ";\n";
		static unsigned int counterSqlStore = 0;
		int storeId = STORE_PROC_ID_REGISTER_1 + 
			      (opt_mysqlstore_max_threads_register > 1 &&
			       sqlStore->getSize(STORE_PROC_ID_CDR_1) > 1000 ? 
				counterSqlStore % opt_mysqlstore_max_threads_register : 
				0);
		++counterSqlStore;
		sqlStore->query_lock(query_str.c_str(), storeId);
	} else {
		reg.add(sqlDbSaveRegister->getIdOrInsert(sql_cdr_ua_table, "id", "ua", cdr_ua), "ua_id");
		sqlDbSaveRegister->insert(register_table, reg);
	}
}

void Register::saveFailedToDb(RegisterState *state, bool force, bool enableBatchIfPossible) {
	if(opt_nocdr) {
		return;
	}
	bool save = false;
	if(state->counter > state->save_at_counter) {
		if(state->counter == 1) {
			saveStateToDb(state);
			save = true;
		} else {
			if(!force && (state->state_to - state->state_from) > NEW_REGISTER_NEW_RECORD_FAILED) {
				state->state_from = state->state_to;
				state->counter -= state->save_at_counter;
				saveStateToDb(state);
				save = true;
			} else if(force || (state->state_to - state->save_at) > NEW_REGISTER_UPDATE_FAILED_PERIOD) {
				if(!sqlDbSaveRegister) {
					sqlDbSaveRegister = createSqlObject();
					sqlDbSaveRegister->setEnableSqlStringInContent(true);
				}
				SqlDb_row row;
				row.add(state->counter, "counter");
				if(enableBatchIfPossible && isSqlDriver("mysql")) {
					string query_str = sqlDbSaveRegister->updateQuery("register_failed", row, 
											  ("ID = " + intToString(state->db_id)).c_str());
					static unsigned int counterSqlStore = 0;
					int storeId = STORE_PROC_ID_REGISTER_1 + 
						      (opt_mysqlstore_max_threads_register > 1 &&
						       sqlStore->getSize(STORE_PROC_ID_CDR_1) > 1000 ? 
							counterSqlStore % opt_mysqlstore_max_threads_register : 
							0);
					++counterSqlStore;
					sqlStore->query_lock(query_str.c_str(), storeId);
				} else {
					sqlDbSaveRegister->update("register_failed", row, 
								  ("ID = " + intToString(state->db_id)).c_str());
				}
				save = true;
			}
		}
	}
	if(save) {
		state->save_at = state->state_to;
		state->save_at_counter = state->counter;
	}
}

eRegisterState Register::getState() {
	lock_states();
	RegisterState *state = states_last();
	eRegisterState rslt_state = state ? state->state : rs_na;
	unlock_states();
	return(rslt_state);
}

u_int32_t Register::getStateFrom() {
	lock_states();
	RegisterState *state = states_last();
	u_int32_t state_from = state->state_from ? state->state_from : 0;
	unlock_states();
	return(state_from);
}

bool Register::getDataRow(RecordArray *rec) {
	lock_states();
	RegisterState *state = states_last();
	if(!state) {
		unlock_states();
		return(false);
	}
	rec->fields[rf_id].set(id);
	rec->fields[rf_sipcallerip].set(htonl(sipcallerip));
	rec->fields[rf_sipcalledip].set(htonl(sipcalledip));
	rec->fields[rf_to_num].set(to_num);
	rec->fields[rf_to_domain].set(to_domain);
	rec->fields[rf_contact_num].set(contact_num);
	rec->fields[rf_contact_domain].set(contact_domain);
	rec->fields[rf_digestusername].set(digest_username);
	if(state->id_sensor >= 0) {
		rec->fields[rf_id_sensor].set(state->id_sensor);
	}
	rec->fields[rf_fname].set(state->fname);
	rec->fields[rf_calldate].set(state->state_from, RecordArrayField::tf_time);
	rec->fields[rf_from_num].set(state->from_num == EQ_REG ? from_num : state->from_num);
	rec->fields[rf_from_name].set(state->from_name == EQ_REG ? from_name : state->from_name);
	rec->fields[rf_from_domain].set(state->from_domain == EQ_REG ? from_domain : state->from_domain);
	rec->fields[rf_digestrealm].set(state->digest_realm == EQ_REG ? digest_realm : state->digest_realm);
	rec->fields[rf_expires].set(state->expires);
	rec->fields[rf_expires_at].set(state->state_from + state->expires, RecordArrayField::tf_time);
	rec->fields[rf_state].set(state->state);
	rec->fields[rf_ua].set(state->ua == EQ_REG ? ua : state->ua);
	if(rrd_count) {
		rec->fields[rf_rrd_avg].set(rrd_sum / rrd_count);
	}
	unlock_states();
	return(true);
}

volatile u_int64_t Register::_id = 0;
volatile int Register::_sync_id = 0;


Registers::Registers() {
	_sync_registers = 0;
	_sync_registers_erase = 0;
	register_failed_id = 0;
	_sync_register_failed_id = 0;
	last_cleanup_time = 0;
}

Registers::~Registers() {
	clean_all();
}

void Registers::add(Call *call) {
 
	/*
	string digest_username_orig = call->digest_username;
	for(int q = 1; q <= 3; q++) {
	sprintf(call->digest_username, "%s-%i", digest_username_orig.c_str(), q);
	*/
	
	if(!convRegisterState(call)) {
		return;
	}
	Register *reg = new FILE_LINE(20004) Register(call);
	/*
	cout 
		<< "* sipcallerip:" << reg->sipcallerip << " / "
		<< "* sipcalledip:" << reg->sipcalledip << " / "
		<< "* to_num:" << (reg->to_num ? reg->to_num : "") << " / "
		<< "* to_domain:" << (reg->to_domain ? reg->to_domain : "") << " / "
		<< "contact_num:" << (reg->contact_num ? reg->contact_num : "") << " / "
		<< "contact_domain:" << (reg->contact_domain ? reg->contact_domain : "") << " / "
		<< "* digest_username:" << (reg->digest_username ? reg->digest_username : "") << " / "
		<< "from_num:" << (reg->from_num ? reg->from_num : "") << " / "
		<< "from_name:" << (reg->from_name ? reg->from_name : "") << " / "
		<< "from_domain:" << (reg->from_domain ? reg->from_domain : "") << " / "
		<< "digest_realm:" << (reg->digest_realm ? reg->digest_realm : "") << " / "
		<< "ua:" << (reg->ua ? reg->ua : "") << endl;
	*/
	RegisterId rid(reg);
	lock_registers();
	map<RegisterId, Register*>::iterator iter = registers.find(rid);
	if(iter == registers.end()) {
		reg->addState(call);
		registers[rid] = reg;
		unlock_registers();
	} else {
		iter->second->update(call);
		unlock_registers();
		iter->second->addState(call);
		delete reg;
	}
	
	/*
	}
	strcpy(call->digest_username, digest_username_orig.c_str());
	*/
	
	cleanup(call->calltime());
	
	/*
	eRegisterState states[] = {
		rs_OK,
		rs_UnknownMessageOK,
		rs_na
	};
	cout << getDataTableJson(states, 0, rf_calldate, false) << endl;
	*/
}

void Registers::cleanup(u_int32_t act_time, bool force) {
	if(!last_cleanup_time) {
		last_cleanup_time = act_time;
		return;
	}
	if(act_time > last_cleanup_time + NEW_REGISTER_CLEAN_PERIOD || force) {
		lock_registers();
		map<RegisterId, Register*>::iterator iter;
		for(iter = registers.begin(); iter != registers.end(); ) {
			Register *reg = iter->second;
			reg->lock_states();
			RegisterState *regstate = reg->states_last();
			bool eraseRegister = false;
			bool eraseRegisterFailed = false;
			if(regstate) {
				if(regstate->state == rs_OK || regstate->state == rs_UnknownMessageOK) {
					if(regstate->expires &&
					   regstate->state_to + regstate->expires < act_time) {
						reg->expire(false);
						// cout << "expire" << endl;
					}
				} else {
					if(regstate->state == rs_Failed) {
						iter->second->saveFailedToDb(regstate, force);
					}
					if(!_sync_registers_erase) {
						if(regstate->state == rs_Failed && reg->countStates == 1 &&
						   regstate->state_to + NEW_REGISTER_ERASE_FAILED_TIMEOUT < act_time) {
							eraseRegisterFailed = true;
							// cout << "erase failed" << endl;
						} else if(regstate->state_to + NEW_REGISTER_ERASE_TIMEOUT < act_time) {
							eraseRegister = true;
							// cout << "erase" << endl;
						}
					}
				}
			}
			reg->unlock_states();
			if(eraseRegister || eraseRegisterFailed) {
				lock_registers_erase();
				delete iter->second;
				registers.erase(iter++);
				unlock_registers_erase();
			} else {
				iter++;
			}
		}
		unlock_registers();
		last_cleanup_time = act_time;
	}
}

void Registers::clean_all() {
	lock_registers();
	while(registers.size()) {
		delete registers.begin()->second;
		registers.erase(registers.begin());
	}
	unlock_registers();
}

u_int64_t Registers::getNewRegisterFailedId(int sensorId) {
	lock_register_failed_id();
	if(!register_failed_id) {
		SqlDb *db = createSqlObject();
		db->query("select max(id) as id from register_failed");
		SqlDb_row row = db->fetchRow();
		if(row) {
			register_failed_id = atoll(row["id"].c_str());
		}
		delete db;
	}
	u_int64_t id = register_failed_id = ((register_failed_id / 100000 + 1) * 100000) + (sensorId >= 0 ? sensorId : 99999);
	unlock_register_failed_id();
	return(id);
}

string Registers::getDataTableJson(char *params, bool *zip) {
 
	JsonItem jsonParams;
	jsonParams.parse(params);
	
	eRegisterState states[10];
	memset(states, 0, sizeof(states));
	unsigned states_count = 0;
	string states_str = jsonParams.getValue("states");
	if(!states_str.empty()) {
		vector<string> states_str_vect = split(states_str, ',');
		for(unsigned i = 0; i < states_str_vect.size(); i++) {
			if(states_str_vect[i] == "OK") {			states[states_count++] = rs_OK;
			} else if(states_str_vect[i] == "Failed") {		states[states_count++] = rs_Failed;
			} else if(states_str_vect[i] == "UnknownMessageOK") {	states[states_count++] = rs_UnknownMessageOK;
			} else if(states_str_vect[i] == "ManyRegMessages") {	states[states_count++] = rs_ManyRegMessages;
			} else if(states_str_vect[i] == "Expired") {		states[states_count++] = rs_Expired;
			} else if(states_str_vect[i] == "Unregister") {		states[states_count++] = rs_Unregister;
			}
		}
	}
	
	u_int32_t limit = atol(jsonParams.getValue("limit").c_str());
	string sortBy = jsonParams.getValue("sort_field");
	eRegisterField sortById = convRegisterFieldToFieldId(sortBy.c_str());
	string sortDir = jsonParams.getValue("sort_dir");
	std::transform(sortDir.begin(), sortDir.end(), sortDir.begin(), ::tolower);
	bool sortDesc = sortDir.substr(0, 4) == "desc";
	
	u_int32_t stateFromLe = atol(jsonParams.getValue("state_from_le").c_str());
	string duplicityOnlyBy = jsonParams.getValue("duplicity_only_by");
	eRegisterField duplicityOnlyById = convRegisterFieldToFieldId(duplicityOnlyBy.c_str());
	string duplicityOnlyCheck = jsonParams.getValue("duplicity_only_check");
	eRegisterField duplicityOnlyCheckId = convRegisterFieldToFieldId(duplicityOnlyCheck.c_str());
	u_int32_t rrdGe = atol(jsonParams.getValue("rrd_ge").c_str());
	
	if(zip) {
		string zipParam = jsonParams.getValue("zip");
		std::transform(zipParam.begin(), zipParam.end(), zipParam.begin(), ::tolower);
		*zip = zipParam == "yes" || zipParam == "yes";
	}
 
	lock_registers_erase();
	lock_registers();
	
	u_int32_t list_registers_size = registers.size();
	u_int32_t list_registers_count = 0;
	Register **list_registers = new FILE_LINE(20005) Register*[list_registers_size];
	
	//cout << "**** 001 " << getTimeMS() << endl;
	
	for(map<RegisterId, Register*>::iterator iter_reg = registers.begin(); iter_reg != registers.end(); iter_reg++) {
		if(states_count) {
			bool okState = false;
			eRegisterState state = iter_reg->second->getState();
			for(unsigned i = 0; i < states_count; i++) {
				if(states[i] == state) {
					okState = true;
					break;
				}
			}
			if(!okState) {
				continue;
			}
		}
		if(stateFromLe) {
			u_int32_t stateFrom = iter_reg->second->getStateFrom();
			if(!stateFrom || stateFrom > stateFromLe) {
				continue;
			}
		}
		if(rrdGe) {
			if(!iter_reg->second->rrd_count ||
			   iter_reg->second->rrd_sum / iter_reg->second->rrd_count < rrdGe) {
				continue;
			}
		}
		list_registers[list_registers_count++] = iter_reg->second;
	}
	
	//cout << "**** 002 " << getTimeMS() << endl;
	
	unlock_registers();
	
	list<RecordArray> records;
	for(unsigned i = 0; i < list_registers_count; i++) {
		RecordArray rec(rf__max);
		if(list_registers[i]->getDataRow(&rec)) {
			rec.sortBy = sortById;
			rec.sortBy2 = rf_id;
			records.push_back(rec);
		}
	}
	delete [] list_registers;
	
	unlock_registers_erase();
	
	string table;
	string header = "[";
	for(unsigned i = 0; i < sizeof(registerFields) / sizeof(registerFields[0]); i++) {
		if(i) {
			header += ",";
		}
		header += '"' + string(registerFields[i].fieldName) + '"';
	}
	header += "]";
	table = "[" + header;
	if(records.size()) {
		string filter = jsonParams.getValue("filter");
		if(!filter.empty()) {
			//cout << "FILTER: " << filter << endl;
			cRegisterFilter *regFilter = new cRegisterFilter(filter.c_str());
			for(list<RecordArray>::iterator iter_rec = records.begin(); iter_rec != records.end(); ) {
				if(!regFilter->check(&(*iter_rec))) {
					iter_rec->free();
					records.erase(iter_rec++);
				} else {
					iter_rec++;
				}
			}
			delete regFilter;
		}
	}
	if(records.size() && duplicityOnlyById && duplicityOnlyCheckId) {
		map<RecordArrayField2, list<RecordArrayField2> > dupl_map;
		map<RecordArrayField2, list<RecordArrayField2> >::iterator dupl_map_iter;
		for(list<RecordArray>::iterator iter_rec = records.begin(); iter_rec != records.end(); iter_rec++) {
			RecordArrayField2 duplBy(&iter_rec->fields[duplicityOnlyById]);
			RecordArrayField2 duplCheck(&iter_rec->fields[duplicityOnlyCheckId]);
			dupl_map_iter = dupl_map.find(duplBy);
			if(dupl_map_iter == dupl_map.end()) {
				dupl_map[duplBy].push_back(duplCheck);
			} else {
				list<RecordArrayField2> *l = &dupl_map_iter->second;
				bool exists = false;
				for(list<RecordArrayField2>::iterator iter = l->begin(); iter != l->begin(); iter++) {
					if(*iter == duplCheck) {
						exists = true;
						break;
					}
				}
				if(!exists) {
					l->push_back(duplCheck);
				}
			}
		}
		for(list<RecordArray>::iterator iter_rec = records.begin(); iter_rec != records.end(); ) {
			RecordArrayField2 duplBy(&iter_rec->fields[duplicityOnlyById]);
			dupl_map_iter = dupl_map.find(duplBy);
			if(dupl_map_iter != dupl_map.end() &&
			   dupl_map_iter->second.size() > 1) {
				iter_rec++;
			} else {
				iter_rec->free();
				records.erase(iter_rec++);
			}
		}
	}
	if(records.size()) {
		table += string(", [{\"total\": ") + intToString(records.size()) + "}]";
		if(sortById) {
			records.sort();
		}
		list<RecordArray>::iterator iter_rec = sortDesc ? records.end() : records.begin();
		if(sortDesc) {
			iter_rec--;
		}
		u_int32_t counter = 0;
		while(counter < records.size() && iter_rec != records.end()) {
			table += "," + iter_rec->getJson();
			if(sortDesc) {
				if(iter_rec != records.begin()) {
					iter_rec--;
				} else {
					break;
				}
			} else {
				iter_rec++;
			}
			++counter;
			if(limit && counter >= limit) {
				break;
			}
		}
		for(iter_rec = records.begin(); iter_rec != records.end(); iter_rec++) {
			iter_rec->free();
		}
	}
	table += "]";
	return(table);
}

void Registers::cleanupByJson(char *params) {

	JsonItem jsonParams;
	jsonParams.parse(params);

	eRegisterState states[10];
	memset(states, 0, sizeof(states));
	unsigned states_count = 0;
	string states_str = jsonParams.getValue("states");
	if(!states_str.empty()) {
		vector<string> states_str_vect = split(states_str, ',');
		for(unsigned i = 0; i < states_str_vect.size(); i++) {
			if(states_str_vect[i] == "OK") {			states[states_count++] = rs_OK;
			} else if(states_str_vect[i] == "Failed") {		states[states_count++] = rs_Failed;
			} else if(states_str_vect[i] == "UnknownMessageOK") {	states[states_count++] = rs_UnknownMessageOK;
			} else if(states_str_vect[i] == "ManyRegMessages") {	states[states_count++] = rs_ManyRegMessages;
			} else if(states_str_vect[i] == "Expired") {		states[states_count++] = rs_Expired;
			} else if(states_str_vect[i] == "Unregister") {		states[states_count++] = rs_Unregister;
			}
		}
	}
	
	lock_registers_erase();
	lock_registers();
	
	for(map<RegisterId, Register*>::iterator iter_reg = registers.begin(); iter_reg != registers.end(); ) {
		bool okState = false;
		if(states_count) {
			eRegisterState state = iter_reg->second->getState();
			for(unsigned i = 0; i < states_count; i++) {
				if(states[i] == state) {
					okState = true;
					break;
				}
			}
		} else {
			okState = true;
		}
		if(okState) {
			delete iter_reg->second;
			registers.erase(iter_reg++);
		} else {
			iter_reg++;
		}
	}
	
	unlock_registers();
	unlock_registers_erase();
}


eRegisterState convRegisterState(Call *call) {
	return(call->msgcount <= 1 ||
	       call->lastSIPresponseNum == 401 || call->lastSIPresponseNum == 403 || call->lastSIPresponseNum == 404 ?
		rs_Failed :
	       call->regstate == 1 && !call->register_expires ?
		rs_Unregister :
		(eRegisterState)call->regstate);
}

eRegisterField convRegisterFieldToFieldId(const char *field) {
	for(unsigned i = 0; i < sizeof(registerFields) / sizeof(registerFields[0]); i++) {
		if(!strcmp(field, registerFields[i].fieldName)) {
			return(registerFields[i].filedType);
		}
	}
	return((eRegisterField)0);
}
