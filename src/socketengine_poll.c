/************************************************************************
 *   IRC - Internet Relay Chat, src/socketengine_poll.c
 *   Copyright (C) 2003 Lucas Madar
 *
 * engine functions for the poll() socket engine
 *
 */

/* $Id$ */

#include "struct.h"
#include "common.h"
#include "sys.h"
#include "h.h"
#include "fds.h"

#include <sys/poll.h>

struct pollfd poll_fds[MAXCONNECTIONS];
int last_pfd = -1;

void engine_init()
{
}

void engine_add_fd(int fd)
{
   struct pollfd *pfd = &poll_fds[++last_pfd];

   /* sanity check */
   if(last_pfd >= MAXCONNECTIONS)
      abort();

   set_fd_internal(fd, (void *) last_pfd);

   pfd->fd = fd;
   pfd->events = 0;
   pfd->revents = 0;
}

void engine_del_fd(int fd)
{
   int arrayidx = (int) get_fd_internal(fd);

   /* If it's at the end of the array, just chop it off */
   if(arrayidx == last_pfd)
   {
      fdfprintf(stderr, "Removing %d[%d] from end of pollfds\n", last_pfd, fd);
      last_pfd--;
      return;
   }

   /* Otherwise, move the last array member to where the old one was */
   fdfprintf(stderr, "Moving pfd %d[%d] to vacated spot %d[%d] -- now %d[%d]\n", 
             last_pfd, poll_fds[last_pfd].fd, arrayidx, fd, last_pfd, fd);
   memcpy(&poll_fds[arrayidx], &poll_fds[last_pfd], sizeof(struct pollfd));
   last_pfd--;
   set_fd_internal(poll_fds[arrayidx].fd, (void *) arrayidx);
}

void engine_change_fd_state(int fd, unsigned int stateplus)
{
   int arrayidx = (int) get_fd_internal(fd);
   struct pollfd *pfd = &poll_fds[arrayidx];

   pfd->events = 0;
   if(stateplus & FDF_WANTREAD)
      pfd->events |= POLLIN|POLLHUP|POLLERR;
   if(stateplus & FDF_WANTWRITE)
      pfd->events |= POLLOUT;
}

void engine_get_pollfds(struct pollfd **pfds, int *numpfds)
{
   *pfds = poll_fds;
   *numpfds = (last_pfd + 1);
}

int engine_read_message(time_t delay)
{
   static char errmsg[512];
   static struct pollfd poll_fdarray[MAXCONNECTIONS];

   struct pollfd *pfd;
   int nfds, nbr_pfds, length, i;
   unsigned int fdflags;
   int fdtype;
   void *fdvalue;
   aClient *cptr;

   engine_get_pollfds(&pfd, &nbr_pfds);
   memcpy(poll_fdarray, pfd, sizeof(struct pollfd) * nbr_pfds);

   nfds = poll(poll_fdarray, nbr_pfds, delay * 1000);
   if (nfds == -1)
   {
      if(((errno == EINTR) || (errno == EAGAIN)))
         return -1;
      report_error("poll %s:%s", &me);
      sleep(5);
      return -1;
   }

   for (pfd = poll_fdarray, i = 0; i < nbr_pfds; i++, pfd++) 
   {
      get_fd_info(pfd->fd, &fdtype, &fdflags, &fdvalue);

      cptr = NULL;
      length = -1;

      if (nfds && pfd->revents)
      {
         int rr = pfd->revents & (POLLIN|POLLHUP|POLLERR);
         int rw = pfd->revents & (POLLOUT);

         fdfprintf(stderr, "fd %d: %s%s\n", pfd->fd, rr ? "read " : "", rw ? "write" : "");

         nfds--;

         switch(fdtype)
         {
            case FDT_NONE:
               continue;

            case FDT_AUTH:
               cptr = (aClient *) fdvalue;
               if (rr)
                  read_authports(cptr);
               if (rw && cptr->authfd >= 0)
                  send_authports(cptr);
               check_client_fd(cptr);
               continue;

            case FDT_LISTENER:
               cptr = (aClient *) fdvalue;
               if(rr)
                  accept_connection(cptr);
               continue;

            case FDT_RESOLVER:
               do_dns_async();
               continue;

            case FDT_CLIENT:
               cptr = (aClient *) fdvalue;

               /*
                * NOTE
                *
                * We now do this in a more logical way.
                * We request a write poll on a socket for two reasons
                * - the socket is waiting for a connect() call
                * - the socket is blocked
                */
               if (rw)
               {
                  if (IsConnecting(cptr) && completed_connection(cptr))
                  {
                     ircsprintf(errmsg, "Connect Error: %s", irc_get_sockerr(cptr));
                     exit_client(cptr, cptr, &me, errmsg);
                     continue;
                  }

                  if(cptr->flags & FLAGS_BLOCKED)
                  {
                     cptr->flags &= ~FLAGS_BLOCKED;
                     unset_fd_flags(cptr->fd, FDF_WANTWRITE);
                  }
               }

               length = 1; /* for fall through case */

               if (rr)
                 length = read_packet(cptr);
               else if(DBufLength(&cptr->recvQ) && IsPerson(cptr) && !NoNewLine(cptr))
                 length = do_client_queue(cptr);
               break;

            default:
               abort(); /* unknown client type? bail! */
         }
      }
      else if(fdtype == FDT_CLIENT)
      {
         cptr = (aClient *) fdvalue;

         if(DBufLength(&cptr->recvQ) && IsPerson(cptr) && !NoNewLine(cptr))
            length = do_client_queue(cptr);
         else
            continue;
      }
      else
         continue; /* we fall through past this if it's a client */

      if (length == FLUSH_BUFFER)
         continue;
	
      if (IsDead(cptr)) 
      {
         ircsprintf(errmsg, "Read/Dead Error: %s", 
                    (cptr->flags & FLAGS_SENDQEX) ?
                    "SendQ Exceeded" : irc_get_sockerr(cptr));
         exit_client(cptr, cptr, &me, errmsg);
         continue;
      }
	
      if (length > 0)
         continue;
	
      /* An error has occured reading from cptr, drop it. */
      read_error_exit(cptr, length, cptr->sockerr);

   } /* end of for() loop for testing polled sockets */

   return 0;
}
