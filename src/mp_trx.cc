
#include "mp_trx.h"
#include "sock_toolkit.h"
#include "busi_base.h"

/* 
 * receive mysql packet 
 */
int 
mp_trx::rx(sock_toolkit *st, epoll_priv_data* priv, int fd)
{
  int ret = 0;
  char *req =0;
  ssize_t szBlk = 0;
  size_t szReq = 0;
  char blk[nMaxRecvBlk], *pblk = 0;
  bool bStop = false ;
  business_base *pb = static_cast<business_base*>(priv->parent);
  tContainer tmp ;

  /* recv a single block of data */
  do {
    pblk= blk;
    ret = do_recv(fd,&pblk,&szBlk,nMaxRecvBlk) ;

    if (ret==MP_ERR) {
      return -1;
    }

    /* if there're pending bytes in cache, means the 
     *  packet in cache is incompleted, so go ahead 
     *  to receive the rest */
    if (is_epp_data_pending(priv)) {
      return 0;
    }

    const char *pBlkEnd = pblk + szBlk ;
    bool relCache = true ;

    /* get requests out of pblk one by one */
    for (req=pblk;!bStop && req<pBlkEnd;req+=szReq) {

      const size_t szRest = pBlkEnd-req ;

      if (szRest<4) {
        log_print("incomplete header %zu on %d\n",szRest,fd);

        /* cache the partial header */
        create_epp_cache(priv,req,szRest,4);

        return 0 ;
      }

      /* current req size pointed to by req */
      szReq = mysqls_get_req_size(req);

      if (szRest<szReq) {

        log_print("incomplete body %zu on %d needs %zu,ret %d\n",
          szRest,fd,szReq,ret);

        /* there're contents in cache already, so 
         *  back it up first */
        if (is_epp_cache_valid(priv)) {
          tmp.tc_resize(szRest+10);
          tmp.tc_write(req,szRest);
          req = tmp.tc_data() ;
        }

        /* resize the cache and wait to receive the rest */
        create_epp_cache(priv,req,szRest,szReq);
        relCache = false ;

        /* continue to read from net and dont release cache */
        if (ret==MP_OK) {
          break ;
        }

        return 0 ;
      }

      if (!mysqls_is_packet_valid(req,szReq)) {
        log_print("myfd %d packet err\n",fd);
        return -1;
      }

      /* process the incoming packet */
      if (pb && pb->deal_pkt(fd,req,szReq,priv->param)) {
        /* TODO: no need to recv any data */
        bStop = true ;
      }

    } /* end for */

    /* all received datas are completely processed, 
     *  try to release cache if there is */
    if (relCache) {
      free_epp_cache(priv);
    }

  } while (!bStop && ret==MP_OK);

  return 0;
}

/* send mysql packet */
int mp_trx::tx(int fd, char *buf, size_t sz)
{
  /* 
   * 1. if there're data in tx cache, try appending them and triger tx events
   *
   * 2. otherwise send them directly and cache the rest
   */
  size_t ln = 0;
  ssize_t rest = 0;
  char *pRest= 0;

  get_tx_cache_data_st(fd,NULL,&ln);

#if 1
  /* XXX: test */
  epoll_priv_data **epp = get_epp(fd), *ep = *epp;

  if (ep->tx_cache.tx_st==0) {
#else
  if (ln==0) {
#endif
    /* no pendings in tx cache, send 'data' directly */
    ssize_t ret = do_send(fd,buf,sz);

    /* the 'rest' data should be appended into tx cache */
    pRest= buf+(ret<0?0:ret) ;
    rest = ret<0?sz:(sz-ret) ;
  } 
  else {
    /* there're pending data in cache, append all into cache */
    pRest= buf ;
    rest = sz ;
  }


  int cnt = 0;

  do {

    /* fit the rest space of tx cache */
    ln = get_tx_cache_free_size_st(fd);
    if ((ssize_t)ln<rest) {
      log_print("warning: not enough spaces %zu in cache, "
        "force flushing\n", ln);
      /* XXX: test */
      flush_tx(fd);
    }
    else {
      break ;
    }

  } while(cnt++<3);

  if (cnt>=3) {
    log_print("error: truncate cache %zu into %zu\n", rest, ln);
    rest = ln ;
  }

  /* cache data & triger tx events */
  if (rest>0) {

    /* XXX: test */
    ln = get_tx_cache_free_size_st(fd);
    log_print("cache %zu bytes fd %d rest %zu\n",rest,fd,ln);

    append_tx_cache(fd,pRest,rest);

    enable_send(NULL,fd);
  }

  return 0;
}

