/*
Serval Distributed Numbering Architecture (DNA)
Copyright (C) 2010 Paul Gardner-Stephen
 
This program is free software; you can redistribute it and/or
modify it under the terms of the GNU General Public License
as published by the Free Software Foundation; either version 2
of the License, or (at your option) any later version.
 
This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.
 
You should have received a copy of the GNU General Public License
along with this program; if not, write to the Free Software
Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
*/

/*
  Smart-flooding of broadcast information is also a requirement.  The long addresses help here, as we can make any address that begins
  with the first 192 bits all ones be broadcast, and use the remaining 64 bits as a "broadcast packet identifier" (BPI).  
  Nodes can remember recently seen BPIs and not forward broadcast frames that have been seen recently.  This should get us smart flooding
  of the majority of a mesh (with some node mobility issues being a factor).  We could refine this later, but it will do for now, especially
  since for things like number resolution we are happy to send repeat requests.
 */

#include "serval.h"
#include "conf.h"
#include "str.h"
#include "overlay_address.h"
#include "overlay_buffer.h"
#include "overlay_packet.h"
#include <arpa/inet.h>

#define MAX_BPIS 1024
#define BPI_MASK 0x3ff
static struct broadcast bpilist[MAX_BPIS];

#define OA_CODE_SELF 0xff
#define OA_CODE_PREVIOUS 0xfe

// each node has 16 slots based on the next 4 bits of a subscriber id
// each slot either points to another tree node or a struct subscriber.
struct tree_node{
  // bit flags for the type of object each element points to
  int is_tree;
  
  union{
    struct tree_node *tree_nodes[16];
    struct subscriber *subscribers[16];
  };
};

static struct tree_node root;

struct subscriber *my_subscriber=NULL;

static unsigned char get_nibble(const unsigned char *sid, int pos){
  unsigned char byte = sid[pos>>1];
  if (!(pos&1))
    byte=byte>>4;
  return byte&0xF;
}

// find a subscriber struct from a whole or abbreviated subscriber id
struct subscriber *find_subscriber(const unsigned char *sid, int len, int create){
  struct tree_node *ptr = &root;
  int pos=0;
  if (len!=SID_SIZE)
    create =0;
  
  do{
    unsigned char nibble = get_nibble(sid, pos++);
    
    if (ptr->is_tree & (1<<nibble)){
      ptr = ptr->tree_nodes[nibble];
      
    }else if(!ptr->subscribers[nibble]){
      // subscriber is not yet known
      
      if (create){
	struct subscriber *ret=(struct subscriber *)malloc(sizeof(struct subscriber));
	memset(ret,0,sizeof(struct subscriber));
	ptr->subscribers[nibble]=ret;
	bcopy(sid, ret->sid, SID_SIZE);
	ret->abbreviate_len=pos;
      }
      return ptr->subscribers[nibble];
      
    }else{
      // there's a subscriber in this slot, does it match the rest of the sid we've been given?
      struct subscriber *ret = ptr->subscribers[nibble];
      if (memcmp(ret->sid,sid,len)==0){
	return ret;
      }
      
      // if we need to insert this subscriber, we have to make a new tree node first
      if (!create)
	return NULL;
      
      // create a new tree node and move the existing subscriber into it
      struct tree_node *new=(struct tree_node *)malloc(sizeof(struct tree_node));
      memset(new,0,sizeof(struct tree_node));
      ptr->tree_nodes[nibble]=new;
      ptr->is_tree |= (1<<nibble);
      
      ptr=new;
      nibble=get_nibble(ret->sid,pos);
      ptr->subscribers[nibble]=ret;
      ret->abbreviate_len=pos+1;
      // then go around the loop again to compare the next nibble against the sid until we find an empty slot.
    }
  }while(pos < len*2);
  
  // abbreviation is not unique
  return NULL;
}

/* 
 Walk the subscriber tree, calling the callback function for each subscriber.
 if start is a valid pointer, the first entry returned will be after this subscriber
 if the callback returns non-zero, the process will stop.
 */
static int walk_tree(struct tree_node *node, int pos, 
	      unsigned char *start, int start_len, 
	      unsigned char *end, int end_len,
	      int(*callback)(struct subscriber *, void *), void *context){
  int i=0, e=16;
  
  if (start && pos < start_len*2){
    i=get_nibble(start,pos);
  }
  
  if (end && pos < end_len*2){
    e=get_nibble(end,pos) +1;
  }
  
  for (;i<e;i++){
    if (node->is_tree & (1<<i)){
      if (walk_tree(node->tree_nodes[i], pos+1, start, start_len, end, end_len, callback, context))
	return 1;
    }else if(node->subscribers[i]){
      if (callback(node->subscribers[i], context))
	return 1;
    }
    // stop comparing the start sid after looking at the first branch of the tree
    start=NULL;
  }
  return 0;
}

/*
 walk the tree, starting at start inclusive, calling the supplied callback function
 */
void enum_subscribers(struct subscriber *start, int(*callback)(struct subscriber *, void *), void *context){
  walk_tree(&root, 0, start->sid, SID_SIZE, NULL, 0, callback, context);
}

// quick test to make sure the specified route is valid.
int subscriber_is_reachable(struct subscriber *subscriber){
  if (!subscriber)
    return REACHABLE_NONE;
  
  int ret = subscriber->reachable;
  
  if (ret==REACHABLE_INDIRECT){
    if (!subscriber->next_hop)
      ret = REACHABLE_NONE;
    
    // avoid infinite recursion...
    else if (!(subscriber->next_hop->reachable & REACHABLE_DIRECT))
      ret = REACHABLE_NONE;
    else{
      int r = subscriber_is_reachable(subscriber->next_hop);
      if (r&REACHABLE_ASSUMED)
	ret = REACHABLE_NONE;
      else if (!(r & REACHABLE_DIRECT))
	ret = REACHABLE_NONE;
    }
  }
  
  if (ret & REACHABLE_DIRECT){
    // make sure the interface is still up
    if (!subscriber->interface)
      ret=REACHABLE_NONE;
    else if (subscriber->interface->state!=INTERFACE_STATE_UP)
      ret=REACHABLE_NONE;
  }
  
  return ret;
}

int set_reachable(struct subscriber *subscriber, int reachable){
  if (subscriber->reachable==reachable)
    return 0;
  subscriber->reachable=reachable;

  // These log messages are for use in tests.  Changing them may break test scripts.
  if (debug&DEBUG_OVERLAYROUTING) {
    switch (reachable) {
      case REACHABLE_NONE:
	DEBUGF("NOT REACHABLE sid=%s", alloca_tohex_sid(subscriber->sid));
	break;
      case REACHABLE_SELF:
	break;
      case REACHABLE_INDIRECT:
	DEBUGF("REACHABLE INDIRECTLY sid=%s", alloca_tohex_sid(subscriber->sid));
	DEBUGF("(via %s, %d)",subscriber->next_hop?alloca_tohex_sid(subscriber->next_hop->sid):"NOONE!"
	       ,subscriber->next_hop?subscriber->next_hop->reachable:0);
	break;
      case REACHABLE_UNICAST:
	DEBUGF("REACHABLE VIA UNICAST sid=%s", alloca_tohex_sid(subscriber->sid));
	break;
      case REACHABLE_BROADCAST:
	DEBUGF("REACHABLE VIA BROADCAST sid=%s", alloca_tohex_sid(subscriber->sid));
	break;
      case REACHABLE_UNICAST|REACHABLE_ASSUMED:
	DEBUGF("ASSUMED REACHABLE VIA UNICAST sid=%s", alloca_tohex_sid(subscriber->sid));
	break;
      case REACHABLE_BROADCAST|REACHABLE_ASSUMED:
	DEBUGF("ASSUMED REACHABLE VIA BROADCAST sid=%s", alloca_tohex_sid(subscriber->sid));
	break;
    }
  }

  /* Pre-emptively send a sas request */
  if (!subscriber->sas_valid && reachable&REACHABLE)
    keyring_send_sas_request(subscriber);

  // Hacky layering violation... send our identity to a directory service
  if (subscriber==directory_service)
    directory_registration();
  
  return 0;
}

// mark the subscriber as reachable via reply unicast packet
int reachable_unicast(struct subscriber *subscriber, overlay_interface *interface, struct in_addr addr, int port){
  if (subscriber->reachable&REACHABLE)
    return WHYF("Subscriber %s is already reachable", alloca_tohex_sid(subscriber->sid));
  
  if (subscriber->node)
    return WHYF("Subscriber %s is already known for overlay routing", alloca_tohex_sid(subscriber->sid));
  
  subscriber->interface = interface;
  subscriber->address.sin_family = AF_INET;
  subscriber->address.sin_addr = addr;
  subscriber->address.sin_port = htons(port);
  set_reachable(subscriber, REACHABLE_UNICAST);
  
  return 0;
}

// load a unicast address from configuration, replace with database??
int load_subscriber_address(struct subscriber *subscriber)
{
  if (subscriber_is_reachable(subscriber)&REACHABLE)
    return 0;
  int i = config_host_list__get(&config.hosts, (const sid_t*)subscriber->sid);
  // No unicast configuration? just return.
  if (i == -1)
    return 1;
  const struct config_host *hostc = &config.hosts.av[i].value;
  overlay_interface *interface = NULL;
  if (*hostc->interface){
    interface = overlay_interface_find_name(hostc->interface);
    if (!interface)
      return -1;
  }
  struct sockaddr_in addr;
  addr.sin_family = AF_INET;
  addr.sin_addr = hostc->address;
  addr.sin_port = htons(hostc->port);
  return overlay_send_probe(subscriber, addr, interface);
}

// generate a new random broadcast address
int overlay_broadcast_generate_address(struct broadcast *addr)
{
  int i;
  for(i=0;i<BROADCAST_LEN;i++) addr->id[i]=random()&0xff;
  return 0;
}

// test if the broadcast address has been seen
int overlay_broadcast_drop_check(struct broadcast *addr)
{
  /* Hash the BPI and see if we have seen it recently.
     If so, drop the frame.
     The occassional failure to supress a broadcast frame is not
     something we are going to worry about just yet.  For byzantine
     robustness it is however required. */
  int bpi_index=0;
  int i;
  for(i=0;i<BROADCAST_LEN;i++)
    {
      bpi_index=((bpi_index<<3)&0xfff8)+((bpi_index>>13)&0x7);
      bpi_index^=addr->id[i];
    }
  bpi_index&=BPI_MASK;
  
  if (memcmp(bpilist[bpi_index].id, addr->id, BROADCAST_LEN)){
    if (debug&DEBUG_BROADCASTS)
      DEBUGF("BPI %s is new", alloca_tohex(addr->id, BROADCAST_LEN));
    bcopy(addr->id, bpilist[bpi_index].id, BROADCAST_LEN);
    return 0; /* don't drop */
  }else{
    if (debug&DEBUG_BROADCASTS)
      DEBUGF("BPI %s is a duplicate", alloca_tohex(addr->id, BROADCAST_LEN));
    return 1; /* drop frame because we have seen this BPI recently */
  }
}

int overlay_broadcast_append(struct overlay_buffer *b, struct broadcast *broadcast)
{
  return ob_append_bytes(b, broadcast->id, BROADCAST_LEN);
}

// append an appropriate abbreviation into the address
int overlay_address_append(struct decode_context *context, struct overlay_buffer *b, struct subscriber *subscriber)
{
  if (!subscriber)
    return WHY("No address supplied");
  
  if (context && subscriber==context->sender){
    if (ob_append_byte(b, OA_CODE_SELF))
      return -1;
    
  }else if(context && subscriber==context->previous){
    if (ob_append_byte(b, OA_CODE_PREVIOUS))
      return -1;
    
  }else{
    int len=SID_SIZE;
    if (subscriber->send_full){
      subscriber->send_full=0;
    }else{
      len=(subscriber->abbreviate_len+2)/2;
      if (subscriber->reachable==REACHABLE_SELF)
	len++;
      if (len>SID_SIZE)
	len=SID_SIZE;
    }
    if (ob_append_byte(b, len))
      return -1;
    if (ob_append_bytes(b, subscriber->sid, len))
      return -1;
  }
  if (context)
    context->previous = subscriber;
  return 0;
}

static int add_explain_response(struct subscriber *subscriber, void *context){
  struct decode_context *response = context;
  if (!response->please_explain){
    response->please_explain = calloc(sizeof(struct overlay_frame),1);
    response->please_explain->payload=ob_new();
    ob_limitsize(response->please_explain->payload, 1024);
  }
  
  // if one of our identities is unknown, 
  // the header of our next payload must include our full sid.
  if (subscriber->reachable==REACHABLE_SELF)
    subscriber->send_full = 1;
  
  // add the whole subscriber id to the payload, stop if we run out of space
  DEBUGF("Adding full sid by way of explanation %s", alloca_tohex_sid(subscriber->sid));
  if (ob_append_byte(response->please_explain->payload, SID_SIZE))
    return 1;
  if (ob_append_bytes(response->please_explain->payload, subscriber->sid, SID_SIZE))
    return 1;
  return 0;
}

static int find_subscr_buffer(struct decode_context *context, struct overlay_buffer *b, int len, struct subscriber **subscriber){
  if (len<=0 || len>SID_SIZE){
    return WHY("Invalid abbreviation length");
  }
  
  unsigned char *id = ob_get_bytes_ptr(b, len);
  if (!id){
    return WHY("Not enough space in buffer to parse address");
  }
  
  if (!subscriber){
    WARN("Could not resolve address, no buffer supplied");
    context->invalid_addresses=1;
    return 0;
  }
  
  *subscriber=find_subscriber(id, len, 1);
  
  if (!*subscriber){
    context->invalid_addresses=1;
    
    // generate a please explain in the passed in context
    
    // add the abbreviation you told me about
    if (!context->please_explain){
      context->please_explain = calloc(sizeof(struct overlay_frame),1);
      context->please_explain->payload=ob_new();
      ob_limitsize(context->please_explain->payload, MDP_MTU);
    }
    
    // And I'll tell you about any subscribers I know that match this abbreviation, 
    // so you don't try to use an abbreviation that's too short in future.
    walk_tree(&root, 0, id, len, id, len, add_explain_response, context);
    
    INFOF("Asking for explanation of %s", alloca_tohex(id, len));
    ob_append_byte(context->please_explain->payload, len);
    ob_append_bytes(context->please_explain->payload, id, len);
    
  }else{
    if (context)
      context->previous=*subscriber;
  }
  return 0;
}

int overlay_broadcast_parse(struct overlay_buffer *b, struct broadcast *broadcast)
{
  return ob_get_bytes(b, broadcast->id, BROADCAST_LEN);
}

// returns 0 = success, -1 = fatal parsing error, 1 = unable to identify address
int overlay_address_parse(struct decode_context *context, struct overlay_buffer *b, struct subscriber **subscriber)
{
  int len = ob_get(b);
  if (len<0)
    return WHY("Buffer too small");
  
  switch(len){
    case OA_CODE_SELF:
      if (!context->sender){
	INFO("Could not resolve address, sender has not been set");
	context->invalid_addresses=1;
      }else{
	*subscriber=context->sender;
	context->previous=context->sender;
      }
      return 0;
      
    case OA_CODE_PREVIOUS:
      if (!context->previous){
	INFO("Unable to decode previous address");
	context->invalid_addresses=1;
      }else{
	*subscriber=context->previous;
      }
      return 0;
  }
  
  return find_subscr_buffer(context, b, len, subscriber);
}

// once we've finished parsing a packet, complete and send a please explain if required.
int send_please_explain(struct decode_context *context, struct subscriber *source, struct subscriber *destination){
  IN();
  struct overlay_frame *frame=context->please_explain;
  if (!frame)
    RETURN(0);
  frame->type = OF_TYPE_PLEASEEXPLAIN;
  
  if (source)
    frame->source = source;
  else
    frame->source = my_subscriber;
  
  frame->source->send_full=1;
  frame->destination = destination;
  
  if (destination && (destination->reachable & REACHABLE)){
    frame->ttl=64;
  }else{
    frame->ttl=1;// how will this work with olsr??
    overlay_broadcast_generate_address(&frame->broadcast_id);
    if (context->interface){
      frame->destination_resolved=1;
      frame->next_hop = destination;
      frame->recvaddr=context->addr;
      frame->interface=context->interface;
    }
  }
  
  frame->queue=OQ_MESH_MANAGEMENT;
  if (!overlay_payload_enqueue(frame))
    RETURN(0);
  op_free(frame);
  RETURN(-1);
}

// process an incoming request for explanation of subscriber abbreviations
int process_explain(struct overlay_frame *frame){
  struct overlay_buffer *b=frame->payload;
  
  struct decode_context context;
  bzero(&context, sizeof context);
  
  while(ob_remaining(b)>0){
    int len = ob_get(b);
    if (len<=0 || len>SID_SIZE)
      return WHY("Badly formatted explain message");
    unsigned char *sid = ob_get_bytes_ptr(b, len);
    if (!sid)
      return WHY("Ran past end of buffer");
    
    if (len==SID_SIZE){
      // This message is also used to inform people of previously unknown subscribers
      // make sure we know this one
      find_subscriber(sid,len,1);
    }else{
      // reply to the sender with all subscribers that match this abbreviation
      INFOF("Sending responses for %s", alloca_tohex(sid, len));
      walk_tree(&root, 0, sid, len, sid, len, add_explain_response, &context);
    }
  }
  
  send_please_explain(&context, frame->destination, frame->source);
  return 0;
}
