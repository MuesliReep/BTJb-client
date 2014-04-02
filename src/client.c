#include <string.h>
#include <stdlib.h>
#include <stdio.h>

#include "cJSON.h"
#include "serial.h"
#include "wallet.h"

void client_send_bloom_filter(const struct wallet *wallet) {

	uint8	filter;
	uint32	filterSize;
	uint32	numHashFuncs;
	uint32	tweak;

	wallet_get_bloom_filter_info(btc->wallet, &filter, &filterSize,
	                             &numHashFuncs, &tweak);

	//Make JSON string

	//Send JSON string to host

}

void client_process_message(){ //TODO: input message

}
