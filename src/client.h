/*
 * client.h
 *
 *  Created on: Apr 2, 2014
 *      Author: peter
 */

#ifndef CLIENT_H_
#define CLIENT_H_

void client_send_bloom_filter(const struct wallet *wallet);
void client_process_message();

#endif /* CLIENT_H_ */
