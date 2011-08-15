#include "mphlr.h"

/*
  We use 256bit Curve25519 addresses in the overlay mesh.  This means we have very large
  addresses to send around the place on a regular basis, which is bad.

  We allow addresses to be shortened to save bandwidth, especially for low-bandwidth links.

  For this purpose we have special address prefixes 0x00 - 0x0f which are not allowed to
  occur in unabbreviated addresses.
  
  0x00      = reserved
  0x01-0x02 = one to two byte address by index.
  0x03      = same as last address.
  0x04      = address matches sender (not currently implemented).
  0x05      = address by prefix of three bytes. 
  0x06      = address by prefix of seven bytes. 
  0x07      = address by prefix of eleven bytes.
  0x08      = full address followed by one-byte index allocation.
  0x09-0x0b = same as 0x05-0x07, but assign single byte index.
  0x0c      = reserved.
  0x0d      = same as 0x07, but assign two byte index.
  0x0e      = full address followed by two-byte index allocation.
  0x0f      = broadcast link-local.
  
  However, in this implementation we not include support for the two-byte 
  index code, as it requires 64Kx32=2MB storage, which is too much to ask
  of a mesh potato or a cheap smart phone.

  All indexed abbreviations may reference a token which is used to keep
  track of the epoch of the abbreviation table.  

  This allows us to maintain multiple abbreviation tables at the same time, so 
  that neighbours we have not spoken to for some time will not be so easily 
  confused, as any one epoch will remain valid for some time after it has 
  ceased to be ammended.

  This also allows us to effectively have multiple 256-entry pages, which will
  be more efficient than using a 16-bit index table in most instances, especially
  since we have the flexibility to reorder frames in an ensemble to minimise the
  total length.

  One table is maintained for all interfaces, as we may have neighbours who are
  reachable via multiple interfaces.  It also helps to minimise memory usage.

  A cache of recently seen addresses is also desirable for conclusively resolving
  abbreviated addresses. Failure will allow the birthday paradox to cause us problems
  and also allow an attack based on searching for private keys that yield colliding
  public key prefixes.  This would allow an attacker to mess with the routing and
  divert the traffic of other nodes to themselves.  Thus some cache or similar policy
  is strongly recommended if a node is going to accept address prefixes, especially if 
  it accepts the shorter prefixes.  Seven and eleven byte prefixes should be reasonably
  resistant to this attack. 

  There is no reason why the address abbreviation table cannot be used as the cache,
  excepting for the question of efficiency, as a naive implementation would require
  a linear search.  However, adding a simple index table can mitigate this, and thus
  presents as a sensible solution.  

  If a node receives a packet with an abbreviation that it cannot resolve an
  abbreviation can ask for clarification, including indicating if it does 
  not support the abbreviation mode specified.

  Abbreviations are link-local, but clarifications will always be requested by
  attempting to contact the abbreviator via normal mesh routing rules.

  We need to take some care so as to allow other crypto-systems, and thus storing the
  crypto-system ID byte, although we still limit addresses to 256 bits, so we always need
  no more than 33 bytes.  In any case, indicating the crypto system is not the problem of
  this section, as we are just concerned with abbreviating and expanding the addresses.

  (A simple solution to the multiple crypto-system problem is to have the receiver try each
  known crypto system when decoding a frame, and remember which addresses use which system.
  Probably a reasonable solution for now.)

  To decode abbreviations supplied by other nodes we need to try to replicate a copy of
  their abbreviation table.  This is where things get memory intensive (about 8KB per node).
  However, we only need the table of one-hop neighbours, and it is reasonable to have a
  constrained set of such tables, with a random replacement algorithm.  This allows the 
  structure to be scaled to accomodate varying memory sizes of devices. 
  
  Very memory constrained devices may elect to cache individual entries rather than tables
  of individual nodes, but we haven't implemented that here.

  If we send only a prefix, then we need to be careful to make sure that receivers
  get some chance to learn the full address if they need it. The question is whether
  abbreviation clarification is more or less efficient than sometimes sending the 
  full address.  Given that we already have need for the clarification process, let's
  go that way, and change the policy later if appropriate.

*/

typedef struct overlay_address_table {
  unsigned char epoch;
  char sids[256][SID_SIZE];
  /* 0x00 = not set, which thus limits us to using only 255 (0x01-0xff) of the indexes for
     storing addresses.
     By spending an extra 256 bytes we reduce, but not eliminate the problem of collisions.
     Will think about a complete solution later.
  */
  unsigned char byfirstbyte[256][2];
  /* next free entry in sid[] */
  unsigned char next_free;
} overlay_address_table;

typedef struct sid {
  unsigned char b[SID_SIZE];
} sid;

typedef struct overlay_address_cache {
  int size;
  int shift; /* Used to calculat lookup function, which is (b[0].b[1].b[2]>>shift) */
  sid *sids; /* one entry per bucket, to keep things simple. */
  /* XXX Should have a means of changing the hash function so that naughty people can't try
     to force our cache to flush with duplicate addresses? 
     But we must use only the first 24 bits of the address due to abbreviation policies, 
     so our options are limited.
     For now the hash will be the first k bits.
  */
} overlay_address_cache;

overlay_address_table *abbrs=NULL;
overlay_address_cache *cache=NULL;

sid overlay_abbreviate_previous_address={{0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0}};

/* The address of the sender of the current frame.
   The ID is used to lookup the abbreviation indexes.
   If the ID is -1, then this means that the sender SID has been set, but not looked up.
   This just saves some time instead of doing the *_id determination each time, when we might not need
   it on most occassions.
*/
sid overlay_abbreviate_current_sender={{0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0}};
int overlay_abbreviate_current_sender_id=-1;

int overlay_abbreviate_cache_address(unsigned char *sid)
{
  if ((!cache)&&OVERLAY_ADDRESS_CACHE_SIZE)
    {
      if (OVERLAY_ADDRESS_CACHE_SIZE>0x1000000) 
	exit(WHY("OVERLAY_ADDRESS_CACHE_SIZE must be no larger than 2^24."));
      if (OVERLAY_ADDRESS_CACHE_SIZE<0)
	exit(WHY("OVERLAY_ADDRESS_CACHE_SIZE must be larger than 0."));

      /* Allocate cache */
      cache=calloc(sizeof(overlay_address_cache),1);
      if (!cache) return 0;
      
      /* Allocate address cache */
      cache->size=OVERLAY_ADDRESS_CACHE_SIZE;
      cache->sids=calloc(sizeof(sid),cache->size);
      if (!cache->sids) { free(cache); return 0; }

      /* Work out the number of bits to shift */
      cache->shift=0;
      while(cache->size>>cache->shift) cache->shift++;
      
      if ((1<<cache->shift)!=cache->size)
	exit(WHY("OVERLAY_ADDRESS_CACHE_SIZE must be a power of two."));
    }

  if (!cache) return 0;

  /* Work out the index in the cache where this address would go */
  int index=((sid[0]<<16)|(sid[0]<<8)|sid[2])>>cache->shift;

  /* Does the stored address match the one we have been passed? */
  if (!bcmp(sid,&cache->sids[index].b[0],SID_SIZE))
    /* Address is already in cache, so return and let the caller know. */
    return 1;

  /* Not yet in cache, so store it */
  bcopy(sid,&cache->sids[index].b[0],SID_SIZE);

  return 0;
}

int overlay_abbreviate_try_byindex(unsigned char *in,char *out,int *ofs,int index)
{
  if(!bcmp(in,abbrs->sids[index],SID_SIZE))
    {
      /* We can encode this address with one byte */      
      out[(*ofs)++]=0x01;
      out[(*ofs)++]=index;
      return 0;
    }
  else
    /* Cannot find index in our node list. */
    return 1;
}

int overlay_abbreviate_address(unsigned char *in,char *out,int *ofs)
{
  int i;
  int wasInCachedP=overlay_abbreviate_cache_address(in);

  if (!in) return WHY("in==NULL");
  if (in[0]<0x10) return WHY("Invalid address - 0x00-0x0f are reserved prefixes.");

  if (!abbrs) {
    // Abbreviation table not setup, so allocate it.
    // Epoch starts at zero. 
    // XXX We have only one simultaneous epoch here, not that it is a problem.
    abbrs=calloc(sizeof(overlay_address_table),1);
    if (!abbrs)
      {
	// Could not allocate abbreviation table, so just output full-length address.
	WHY("calloc() failed.");
	bcopy(in,&out[*ofs],SID_SIZE);
	(*ofs)+=SID_SIZE;
	return 0;
      }
    abbrs->next_free=1;
  }

  /* Try abbreviating by index
     XXX should search backwards through old epochs
     XXX If we do, we need a way to indicate a reference to an old epoch */
  for(i=0;i<2;i++)
    if (abbrs->byfirstbyte[in[0]][i])
      { if (!overlay_abbreviate_try_byindex(in,out,ofs,i)) return 0; }
    else break;
  if (i<2&&abbrs->next_free) {
    // There is a spare slot to abbreviate this address by storing it in an index if we 
    // wish. So let's store it, then send the full address along with the newly allocated
    // index.

    /* Remember this address */
    bcopy(in,abbrs->sids[abbrs->next_free],SID_SIZE);
    abbrs->byfirstbyte[in[0]][i]=abbrs->next_free;

    /* Write address out with index code */
    out[(*ofs)++]=0x08;
    bcopy(in,&out[(*ofs)],SID_SIZE);
    (*ofs)+=SID_SIZE;
    out[(*ofs)++]=abbrs->next_free;

    /* Tidy things up and return triumphant. */
    abbrs->next_free++;

    return 0;
  }

  /* No space in our table, so either send address verbatim, or send only a prefix.
     Go for prefix. Next question is length of prefix. Seven bytes is probably about
     right as an simple initial policy. */
  if (wasInCachedP) {
    /* Prefix addresses that have been seen recently */
    out[(*ofs)++]=0x06;
    bcopy(in,&out[(*ofs)],7);
    (*ofs)+=7;
    return 0;
  } else {
    /* But send full address for those we haven't seen before */
    bcopy(in,&out[*ofs],SID_SIZE);
    (*ofs)+=SID_SIZE;
    return 0;
  }

}

int overlay_abbreviate_expand_address(unsigned char *in,int *inofs,unsigned char *out,int *ofs)
{
  int bytes=0;
  switch(in[0])
    {
    case OA_CODE_00: case OA_CODE_02: case OA_CODE_04: case OA_CODE_0C:
      /* Unsupported codes, so tell the sender 
	 if the frame was addressed to us as next-hop */
      (*inofs)++;
      return OA_UNSUPPORTED;
    case OA_CODE_INDEX: /* single byte index look up */
      exit(WHY("Unimplemented address abbreviation code"));
      break;
    case OA_CODE_PREVIOUS: /* Same as last address */
      (*inofs)++;
      bcopy(&overlay_abbreviate_previous_address.b[0],&out[*ofs],SID_SIZE);
      (*ofs)+=SID_SIZE;
      return OA_RESOLVED;
      break;
    case OA_CODE_PREFIX3: case OA_CODE_PREFIX3_INDEX1: /* 3-byte prefix */
      if (in[0]==0x09) bytes=1;
      (*inofs)+=3+bytes;
      return overlay_abbreviate_cache_lookup(in,out,ofs,3,bytes);
    case OA_CODE_PREFIX7: case OA_CODE_PREFIX7_INDEX1: /* 7-byte prefix */
      if (in[0]==OA_CODE_PREFIX7_INDEX1) bytes=1;
      (*inofs)+=7+bytes;
      return overlay_abbreviate_cache_lookup(in,out,ofs,7,bytes);
    case OA_CODE_PREFIX11: case OA_CODE_PREFIX11_INDEX1: case OA_CODE_PREFIX11_INDEX2: /* 11-byte prefix */
      if (in[0]==OA_CODE_PREFIX11_INDEX1) bytes=1;
      if (in[0]==OA_CODE_PREFIX11_INDEX2) bytes=2;
      (*inofs)+=11+bytes;
      return overlay_abbreviate_cache_lookup(in,out,ofs,11,bytes);
    case OA_CODE_BROADCAST: /* broadcast */
      memset(&out[*ofs],0xff,SID_SIZE);
      (*inofs)++;
      return OA_RESOLVED;
    case OA_CODE_FULL_INDEX1: case OA_CODE_FULL_INDEX2: 
    default: /* Full address, optionally followed by index for us to remember */
      if (in[0]==OA_CODE_FULL_INDEX1) bytes=1; 
      if (in[0]==OA_CODE_FULL_INDEX2) bytes=2;
      bcopy(in,&out[*ofs],SID_SIZE);
      if (bytes) overlay_abbreviate_remember_index(bytes,in,&in[SID_SIZE]);
      (*inofs)+=SID_SIZE+bytes;
      return OA_RESOLVED;
    }
}

int overlay_abbreviate_remember_index(int index_byte_count,unsigned char *in,unsigned char *index_bytes)
{
  int zero=0;
  char sid[SID_SIZE*2+1];
  int index=index_bytes[0];
  if (index_byte_count>1) index=(index<<8)|index_bytes[1];

  sid[0]=0; extractSid(in,&zero,sid);
  fprintf(stderr,"We need to remember that the sender #%d has assigned index #%d to the following:\n      [%s]\n",
	  overlay_abbreviate_current_sender_id,index,sid);
  return WHY("Not implemented");
}

int overlay_abbreviate_cache_lookup(unsigned char *in,unsigned char *out,int *ofs,
				    int prefix_bytes,int index_bytes)
{
  /* Lookup this entry from the cache, and also assign it the specified prefix */
  
  /* Work out the index in the cache where this address would live */
  int index=((in[0]<<16)|(in[0]<<8)|in[2])>>cache->shift;

  /* So is it there? */
  if (bcmp(in,&cache->sids[index].b[0],prefix_bytes))
    {
      /* No, it isn't in the cache. */
      return OA_PLEASEEXPLAIN;
    }
  
  /* XXX We should implement associativity in the address cache so that we can spot
     colliding prefixes and ask the sender to resolve them for us */

  /* It is here, so let's return it */
  bcopy(&cache->sids[index].b[0],&out[(*ofs)],SID_SIZE);
  (*ofs)+=SID_SIZE;
  if (index_bytes) {
    /* We need to remember it as well, so do that.
       If this process fails, it is okay, as we can still resolve the address now.
       It will probably result in waste later though when we get asked to look it up,
       however the alternative definitely wastes bandwidth now, so let us defer the
       corrective action in case it is never required. 
    */
    overlay_abbreviate_remember_index(index_bytes,&cache->sids[index].b[0],&in[prefix_bytes]);
  }
  return OA_RESOLVED;
}

int overlay_abbreviate_set_current_sender(char *in)
{
  bcopy(in,&overlay_abbreviate_current_sender.b[0],SID_SIZE);
  overlay_abbreviate_current_sender_id=-1;
  return 0;
}

int overlay_abbreviate_set_most_recent_address(char *in)
{
  bcopy(in,&overlay_abbreviate_previous_address.b[0],SID_SIZE);
  return 0;
}