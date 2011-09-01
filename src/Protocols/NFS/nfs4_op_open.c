/*
 * vim:expandtab:shiftwidth=8:tabstop=8:
 *
 * Copyright CEA/DAM/DIF  (2008)
 * contributeur : Philippe DENIEL   philippe.deniel@cea.fr
 *                Thomas LEIBOVICI  thomas.leibovici@cea.fr
 *
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 3 of the License, or (at your option) any later version.
 * 
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 * 
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 * 
 * ---------------------------------------
 */

/**
 * \file    nfs4_op_open.c
 * \author  $Author: deniel $
 * \date    $Date: 2005/11/28 17:02:51 $
 * \version $Revision: 1.18 $
 * \brief   Routines used for managing the NFS4 COMPOUND functions.
 *
 * nfs4_op_open.c : Routines used for managing the NFS4 COMPOUND functions.
 *
 *
 */
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#ifdef _SOLARIS
#include "solaris_port.h"
#endif

#include <stdio.h>
#include <string.h>
#include <pthread.h>
#include "HashData.h"
#include "HashTable.h"
#include "rpc.h"
#include "log_macros.h"
#include "stuff_alloc.h"
#include "nfs4.h"
#include "nfs_core.h"
#include "sal_functions.h"
#include "nfs_proto_functions.h"
#include "nfs_proto_tools.h"

/**
 * nfs4_op_open: NFS4_OP_OPEN, opens and eventually creates a regular file.
 * 
 * NFS4_OP_OPEN, opens and eventually creates a regular file.
 *
 * @param op    [IN]    pointer to nfs4_op arguments
 * @param data  [INOUT] Pointer to the compound request's data
 * @param resp  [IN]    Pointer to nfs4_op results
 *
 * @return NFS4_OK if successfull, other values show an error.  
 * 
 */

#define arg_OPEN4 op->nfs_argop4_u.opopen
#define res_OPEN4 resp->nfs_resop4_u.opopen

int nfs4_op_open(struct nfs_argop4 *op, compound_data_t * data, struct nfs_resop4 *resp)
{
  char __attribute__ ((__unused__)) funcname[] = "nfs4_op_open";

  cache_entry_t           * pentry_parent = NULL;
  cache_entry_t           * pentry_lookup = NULL;
  cache_entry_t           * pentry_newfile = NULL;
  fsal_handle_t           * pnewfsal_handle = NULL;
  fsal_attrib_list_t        attr_parent;
  fsal_attrib_list_t        attr;
  fsal_attrib_list_t        attr_newfile;
  fsal_attrib_list_t        sattr;
  fsal_openflags_t          openflags = 0;
  cache_inode_status_t      cache_status;
  state_status_t            state_status;
  int                       retval;
  fsal_name_t               filename;
  bool_t                    AttrProvided = FALSE;
  fsal_accessmode_t         mode = 0600;
  nfs_fh4                   newfh4;
  char                      newfh4_val[NFS4_FHSIZE];
  nfs_client_id_t           nfs_clientid;
  nfs_worker_data_t       * pworker = NULL;
  int                       convrc = 0;
  state_data_t              candidate_data;
  state_type_t              candidate_type;
  state_t                 * pfile_state = NULL;
  state_t                 * pstate_found_iterate = NULL;
  state_t                 * pstate_previous_iterate = NULL;
  state_nfs4_owner_name_t   owner_name;
  state_owner_t           * powner = NULL;
  const char              * tag = "OPEN";

  newfh4.nfs_fh4_val = newfh4_val;

  fsal_accessflags_t write_access = FSAL_MODE_MASK_SET(FSAL_W_OK) |
                                    FSAL_ACE4_MASK_SET(FSAL_ACE_PERM_WRITE_DATA |
                                                       FSAL_ACE_PERM_APPEND_DATA);
  fsal_accessflags_t read_access = FSAL_MODE_MASK_SET(FSAL_R_OK) |
                                   FSAL_ACE4_MASK_SET(FSAL_ACE_PERM_READ_DATA);

  resp->resop = NFS4_OP_OPEN;
  res_OPEN4.status = NFS4_OK;

  uint32_t tmp_attr[2];
  uint_t tmp_int = 2;

  pworker = (nfs_worker_data_t *) data->pclient->pworker;

  /* If there is no FH */
  if(nfs4_Is_Fh_Empty(&(data->currentFH)))
    {
      res_OPEN4.status = NFS4ERR_NOFILEHANDLE;
      return res_OPEN4.status;
    }

  /* If the filehandle is invalid */
  if(nfs4_Is_Fh_Invalid(&(data->currentFH)))
    {
      res_OPEN4.status = NFS4ERR_BADHANDLE;
      return res_OPEN4.status;
    }

  /* Tests if the Filehandle is expired (for volatile filehandle) */
  if(nfs4_Is_Fh_Expired(&(data->currentFH)))
    {
      res_OPEN4.status = NFS4ERR_FHEXPIRED;
      return res_OPEN4.status;
    }

  /* This can't be done on the pseudofs */
  if(nfs4_Is_Fh_Pseudo(&(data->currentFH)))
    {
      res_OPEN4.status = NFS4ERR_ROFS;
      return res_OPEN4.status;
    }

  /* If Filehandle points to a xattr object, manage it via the xattrs specific functions */
  if(nfs4_Is_Fh_Xattr(&(data->currentFH)))
    return nfs4_op_open_xattr(op, data, resp);

  /* If data->current_entry is empty, repopulate it */
  if(data->current_entry == NULL)
    {
      if((data->current_entry = nfs_FhandleToCache(NFS_V4,
                                                   NULL,
                                                   NULL,
                                                   &(data->currentFH),
                                                   NULL,
                                                   NULL,
                                                   &(res_OPEN4.status),
                                                   &attr,
                                                   data->pcontext,
                                                   data->pclient,
                                                   data->ht, &retval)) == NULL)
        {
          res_OPEN4.status = NFS4ERR_RESOURCE;
          return res_OPEN4.status;
        }
    }

  /* Set parent */
  pentry_parent = data->current_entry;

  /* First switch is based upon claim type */
  switch (arg_OPEN4.claim.claim)
    {
    case CLAIM_DELEGATE_CUR:
    case CLAIM_DELEGATE_PREV:
      /* Check for name length */
      if(arg_OPEN4.claim.open_claim4_u.file.utf8string_len > FSAL_MAX_NAME_LEN)
        {
          res_OPEN4.status = NFS4ERR_NAMETOOLONG;
          return res_OPEN4.status;
        }

      /* get the filename from the argument, it should not be empty */
      if(arg_OPEN4.claim.open_claim4_u.file.utf8string_len == 0)
        {
          res_OPEN4.status = NFS4ERR_INVAL;
          return res_OPEN4.status;
        }

      res_OPEN4.status = NFS4ERR_NOTSUPP;
      return res_OPEN4.status;

    case CLAIM_NULL:
      /* Is this open_owner known? If so, get it so we can use replay cache */
      convert_nfs4_owner(&arg_OPEN4.owner, &owner_name);

      if(!nfs4_owner_Get_Pointer(&owner_name, &powner))
        {
          LogFullDebug(COMPONENT_NFS_V4_LOCK,
                       "OPEN new owner");
        }
      else
        {
          LogFullDebug(COMPONENT_NFS_V4_LOCK,
                       "A previously known open_owner is used :#%s# seqid=%u arg_OPEN4.seqid=%u",
                       powner->so_owner_val, powner->so_owner.so_nfs4_owner.so_seqid, arg_OPEN4.seqid);

          if(arg_OPEN4.seqid == 0)
            {
              LogDebug(COMPONENT_NFS_V4_LOCK,
                       "Previously known open_owner is used with seqid=0, ask the client to confirm it again");
              powner->so_owner.so_nfs4_owner.so_confirmed = FALSE;
            }
          else
            {
              /* Check for replay */
              if(!Check_nfs4_seqid(powner, arg_OPEN4.seqid, op, data, resp, tag))
                {
                  /* Response is all setup for us and LogDebug told what was wrong */
                  return res_OPEN4.status;
                }
            }
        }

      /* Check for name length */
      if(arg_OPEN4.claim.open_claim4_u.file.utf8string_len > FSAL_MAX_NAME_LEN)
        {
          res_OPEN4.status = NFS4ERR_NAMETOOLONG;

          /* Save the response in the open owner */
          Copy_nfs4_state_req(powner, arg_OPEN4.seqid, op, data, resp, tag);

          return res_OPEN4.status;
        }

      /* get the filename from the argument, it should not be empty */
      if(arg_OPEN4.claim.open_claim4_u.file.utf8string_len == 0)
        {
          res_OPEN4.status = NFS4ERR_INVAL;

          /* Save the response in the open owner */
          Copy_nfs4_state_req(powner, arg_OPEN4.seqid, op, data, resp, tag);

          return res_OPEN4.status;
        }

      /* Check if asked attributes are correct */
      if(arg_OPEN4.openhow.openflag4_u.how.mode == GUARDED4 ||
         arg_OPEN4.openhow.openflag4_u.how.mode == UNCHECKED4)
        {
          if(!nfs4_Fattr_Supported
             (&arg_OPEN4.openhow.openflag4_u.how.createhow4_u.createattrs))
            {
              res_OPEN4.status = NFS4ERR_ATTRNOTSUPP;

              /* Save the response in the open owner */
              Copy_nfs4_state_req(powner, arg_OPEN4.seqid, op, data, resp, tag);

              return res_OPEN4.status;
            }

          /* Do not use READ attr, use WRITE attr */
          if(!nfs4_Fattr_Check_Access
             (&arg_OPEN4.openhow.openflag4_u.how.createhow4_u.createattrs,
              FATTR4_ATTR_WRITE))
            {
              res_OPEN4.status = NFS4ERR_INVAL;

              /* Save the response in the open owner */
              Copy_nfs4_state_req(powner, arg_OPEN4.seqid, op, data, resp, tag);

              return res_OPEN4.status;
            }
        }

      /* Check if filename is correct */
      if((cache_status =
          cache_inode_error_convert(FSAL_buffdesc2name
                                    ((fsal_buffdesc_t *) & arg_OPEN4.claim.open_claim4_u.
                                     file, &filename))) != CACHE_INODE_SUCCESS)
        {
          res_OPEN4.status = nfs4_Errno(cache_status);

          /* Save the response in the open owner */
          Copy_nfs4_state_req(powner, arg_OPEN4.seqid, op, data, resp, tag);

          return res_OPEN4.status;
        }

      /* Check parent */
      pentry_parent = data->current_entry;

      /* Parent must be a directory */
      if((pentry_parent->internal_md.type != DIR_BEGINNING) &&
         (pentry_parent->internal_md.type != DIR_CONTINUE))
        {
          /* Parent object is not a directory... */
          if(pentry_parent->internal_md.type == SYMBOLIC_LINK)
            res_OPEN4.status = NFS4ERR_SYMLINK;
          else
            res_OPEN4.status = NFS4ERR_NOTDIR;

          /* Save the response in the open owner */
          Copy_nfs4_state_req(powner, arg_OPEN4.seqid, op, data, resp, tag);

          return res_OPEN4.status;
        }

      /* What kind of open is it ? */
      LogFullDebug(COMPONENT_NFS_V4_LOCK,
                   "OPEN: Claim type = %d   Open Type = %d  Share Deny = %d   Share Access = %d ",
                   arg_OPEN4.claim.claim,
                   arg_OPEN4.openhow.opentype,
                   arg_OPEN4.share_deny,
                   arg_OPEN4.share_access);

      /* It this a known client id ? */
      LogDebug(COMPONENT_NFS_V4_LOCK,
               "OPEN Client id = %"PRIx64,
               arg_OPEN4.owner.clientid);
      if(nfs_client_id_get(arg_OPEN4.owner.clientid, &nfs_clientid) != CLIENT_ID_SUCCESS)
        {
          res_OPEN4.status = NFS4ERR_STALE_CLIENTID;
          return res_OPEN4.status;
        }

      /* The client id should be confirmed */
      if(nfs_clientid.confirmed != CONFIRMED_CLIENT_ID)
        {
          res_OPEN4.status = NFS4ERR_STALE_CLIENTID;
          return res_OPEN4.status;
        }

      /* Is this open_owner known ? */
      if(powner == NULL)
        {
          /* This open owner is not known yet, allocated and set up a new one */
          powner = create_nfs4_owner(data->pclient,
                                     &owner_name,
                                     &arg_OPEN4.owner,
                                     NULL,
                                     0);

          if(powner == NULL)
            {
              res_OPEN4.status = NFS4ERR_RESOURCE;
              return res_OPEN4.status;
            }
        }

      /* Status of parent directory before the operation */
      if(cache_inode_getattr(pentry_parent,
                             &attr_parent,
                             data->ht,
                             data->pclient,
                             data->pcontext,
                             &cache_status) != CACHE_INODE_SUCCESS)
        {
          res_OPEN4.status = nfs4_Errno(cache_status);

          /* Save the response in the open owner */
          Copy_nfs4_state_req(powner, arg_OPEN4.seqid, op, data, resp, tag);

          return res_OPEN4.status;
        }

      memset(&(res_OPEN4.OPEN4res_u.resok4.cinfo.before), 0, sizeof(changeid4));
      res_OPEN4.OPEN4res_u.resok4.cinfo.before =
          (changeid4) pentry_parent->internal_md.mod_time;

      /* CLient may have provided fattr4 to set attributes at creation time */
      if(arg_OPEN4.openhow.openflag4_u.how.mode == GUARDED4 ||
         arg_OPEN4.openhow.openflag4_u.how.mode == UNCHECKED4)
        {
          if(arg_OPEN4.openhow.openflag4_u.how.createhow4_u.createattrs.attrmask.
             bitmap4_len != 0)
            {
              /* Convert fattr4 so nfs4_sattr */
              convrc =
                  nfs4_Fattr_To_FSAL_attr(&sattr,
                                          &(arg_OPEN4.openhow.openflag4_u.how.
                                            createhow4_u.createattrs));

              if(convrc != NFS4_OK)
                {
                  res_OPEN4.status = convrc;

                  /* Save the response in the open owner */
                  Copy_nfs4_state_req(powner, arg_OPEN4.seqid, op, data, resp, tag);

                  return res_OPEN4.status;
                }

              AttrProvided = TRUE;
            }
        }

      /* Second switch is based upon "openhow" */
      switch (arg_OPEN4.openhow.opentype)
        {
        case OPEN4_CREATE:
          /* a new file is to be created */

          /* Does a file with this name already exist ? */
          pentry_lookup = cache_inode_lookup(pentry_parent,
                                             &filename,
                                             &attr_newfile,
                                             data->ht,
                                             data->pclient,
                                             data->pcontext, &cache_status);

          if(cache_status != CACHE_INODE_NOT_FOUND)
            {
              /* if open is UNCHECKED, return NFS4_OK (RFC3530 page 172) */
              if(arg_OPEN4.openhow.openflag4_u.how.mode == UNCHECKED4
                 && (cache_status == CACHE_INODE_SUCCESS))
                {
                  /* If the file is opened for write, OPEN4 while deny share write access,
                   * in this case, check caller has write access to the file */
                  if(arg_OPEN4.share_deny & OPEN4_SHARE_DENY_WRITE)
                    {
                      if(cache_inode_access(pentry_lookup,
                                            write_access,
                                            data->ht,
                                            data->pclient,
                                            data->pcontext,
                                            &cache_status) != CACHE_INODE_SUCCESS)
                        {
                          res_OPEN4.status = NFS4ERR_ACCESS;

                          /* Save the response in the open owner */
                          Copy_nfs4_state_req(powner, arg_OPEN4.seqid, op, data, resp, tag);

                          return res_OPEN4.status;
                        }
                      openflags = FSAL_O_WRONLY;
                    }

                  /* Same check on read: check for readability of a file before opening it for read */
                  if(arg_OPEN4.share_access & OPEN4_SHARE_ACCESS_READ)
                    {
                      if(cache_inode_access(pentry_lookup,
                                            read_access,
                                            data->ht,
                                            data->pclient,
                                            data->pcontext,
                                            &cache_status) != CACHE_INODE_SUCCESS)
                        {
                          res_OPEN4.status = NFS4ERR_ACCESS;

                          /* Save the response in the open owner */
                          Copy_nfs4_state_req(powner, arg_OPEN4.seqid, op, data, resp, tag);

                          return res_OPEN4.status;
                        }
                      openflags = FSAL_O_RDONLY;
                    }

                  if(AttrProvided == TRUE)      /* Set the attribute if provided */
                    {
                      if(cache_inode_setattr(pentry_lookup,
                                             &sattr,
                                             data->ht,
                                             data->pclient,
                                             data->pcontext,
                                             &cache_status) != CACHE_INODE_SUCCESS)
                        {
                          res_OPEN4.status = nfs4_Errno(cache_status);

                          /* Save the response in the open owner */
                          Copy_nfs4_state_req(powner, arg_OPEN4.seqid, op, data, resp, tag);

                          return res_OPEN4.status;
                        }

                      res_OPEN4.OPEN4res_u.resok4.attrset =
                          arg_OPEN4.openhow.openflag4_u.how.createhow4_u.createattrs.
                          attrmask;
                    }
                  else
                    res_OPEN4.OPEN4res_u.resok4.attrset.bitmap4_len = 0;

                  /* Same check on write */
                  if(arg_OPEN4.share_access & OPEN4_SHARE_ACCESS_WRITE)
                    {
                      if(cache_inode_access(pentry_lookup,
                                            write_access,
                                            data->ht,
                                            data->pclient,
                                            data->pcontext,
                                            &cache_status) != CACHE_INODE_SUCCESS)
                        {
                          res_OPEN4.status = NFS4ERR_ACCESS;

                          /* Save the response in the open owner */
                          Copy_nfs4_state_req(powner, arg_OPEN4.seqid, op, data, resp, tag);

                          return res_OPEN4.status;
                        }
                      openflags = FSAL_O_RDWR;
                    }

                  /* Set the state for the related file */

                  /* Prepare state management structure */
                  candidate_type = STATE_TYPE_SHARE;
                  candidate_data.share.share_deny = arg_OPEN4.share_deny;
                  candidate_data.share.share_access = arg_OPEN4.share_access;
                  candidate_data.share.lockheld = 0;

                  if(state_add(pentry_lookup,
                               candidate_type,
                               &candidate_data,
                               powner,
                               data->pclient,
                               data->pcontext,
                               &pfile_state,
                               &state_status) != STATE_SUCCESS)
                    {
                      res_OPEN4.status = NFS4ERR_SHARE_DENIED;

                      /* Save the response in the open owner */
                      Copy_nfs4_state_req(powner, arg_OPEN4.seqid, op, data, resp, tag);

                      return res_OPEN4.status;
                    }

                  /* Open the file */
                  if(cache_inode_open_by_name(pentry_parent,
                                              &filename,
                                              pentry_lookup,
                                              data->pclient,
                                              openflags,
                                              data->pcontext,
                                              &cache_status) != CACHE_INODE_SUCCESS)
                    {
                      // TODO FSF: huh????
                      res_OPEN4.status = NFS4ERR_SHARE_DENIED;
                      res_OPEN4.status = NFS4ERR_ACCESS;

                      /* Save the response in the open owner */
                      Copy_nfs4_state_req(powner, arg_OPEN4.seqid, op, data, resp, tag);

                      return res_OPEN4.status;
                    }

                  res_OPEN4.OPEN4res_u.resok4.attrset.bitmap4_len = 2;
                  if((res_OPEN4.OPEN4res_u.resok4.attrset.bitmap4_val =
                      (uint32_t *) Mem_Alloc(res_OPEN4.OPEN4res_u.resok4.attrset.
                                             bitmap4_len * sizeof(uint32_t))) == NULL)
                    {
                      res_OPEN4.status = NFS4ERR_RESOURCE;
                      res_OPEN4.OPEN4res_u.resok4.attrset.bitmap4_len = 0;
                      return res_OPEN4.status;
                    }

                  memset(&(res_OPEN4.OPEN4res_u.resok4.cinfo.after), 0,
                         sizeof(changeid4));
                  res_OPEN4.OPEN4res_u.resok4.cinfo.after =
                      (changeid4) pentry_parent->internal_md.mod_time;
                  res_OPEN4.OPEN4res_u.resok4.cinfo.atomic = TRUE;

                  /* No delegation */
                  res_OPEN4.OPEN4res_u.resok4.delegation.delegation_type =
                      OPEN_DELEGATE_NONE;

                  /* If server use OPEN_CONFIRM4, set the correct flag */
                  P(powner->so_mutex);
                  if(powner->so_owner.so_nfs4_owner.so_confirmed == FALSE)
                    {
                      if(nfs_param.nfsv4_param.use_open_confirm == TRUE)
                        res_OPEN4.OPEN4res_u.resok4.rflags =
                            OPEN4_RESULT_CONFIRM + OPEN4_RESULT_LOCKTYPE_POSIX;
                      else
                        res_OPEN4.OPEN4res_u.resok4.rflags = OPEN4_RESULT_LOCKTYPE_POSIX;
                    }
                  V(powner->so_mutex);

                  /* Now produce the filehandle to this file */
                  if((pnewfsal_handle =
                      cache_inode_get_fsal_handle(pentry_lookup, &cache_status)) == NULL)
                    {
                      res_OPEN4.status = nfs4_Errno(cache_status);

                      /* Save the response in the open owner */
                      Copy_nfs4_state_req(powner, arg_OPEN4.seqid, op, data, resp, tag);

                      return res_OPEN4.status;
                    }

                  /* Building a new fh */
                  if(!nfs4_FSALToFhandle(&newfh4, pnewfsal_handle, data))
                    {
                      res_OPEN4.status = NFS4ERR_SERVERFAULT;

                      Copy_nfs4_state_req(powner, arg_OPEN4.seqid, op, data, resp, tag);

                      return res_OPEN4.status;
                    }

                  /* This new fh replaces the current FH */
                  data->currentFH.nfs_fh4_len = newfh4.nfs_fh4_len;
                  memcpy(data->currentFH.nfs_fh4_val, newfh4.nfs_fh4_val,
                         newfh4.nfs_fh4_len);

                  /* No do not need newfh any more */
                  Mem_Free((char *)newfh4.nfs_fh4_val);

                  data->current_entry = pentry_lookup;
                  data->current_filetype = REGULAR_FILE;

                  res_OPEN4.status = NFS4_OK;
                  
                  /* Handle stateid/seqid for success */
                  update_stateid(pfile_state,
                                 &res_OPEN4.OPEN4res_u.resok4.stateid,
                                 data,
                                 "LOCK");

                  /* Save the response in the open owner */
                  Copy_nfs4_state_req(powner, arg_OPEN4.seqid, op, data, resp, tag);

                  return res_OPEN4.status;
                }

              /* if open is EXCLUSIVE, but verifier is the same, return NFS4_OK (RFC3530 page 173) */
              if(arg_OPEN4.openhow.openflag4_u.how.mode == EXCLUSIVE4)
                {
                  if((pentry_lookup != NULL)
                     && (pentry_lookup->internal_md.type == REGULAR_FILE))
                    {
                      pstate_found_iterate = NULL;
                      pstate_previous_iterate = NULL;

                      do
                        {
                          state_iterate(pentry_lookup,
                                        &pstate_found_iterate,
                                        pstate_previous_iterate,
                                        data->pclient,
                                        data->pcontext, &state_status);

                          if(state_status == STATE_STATE_ERROR)
                            break;

                          if(state_status == STATE_INVALID_ARGUMENT)
                            {
                              res_OPEN4.status = NFS4ERR_INVAL;

                              /* Save the response in the open owner */
                              Copy_nfs4_state_req(powner, arg_OPEN4.seqid, op, data, resp, tag);

                              return res_OPEN4.status;
                            }

                          cache_status = CACHE_INODE_SUCCESS;

                          /* Check is open_owner is the same */
                          if(pstate_found_iterate != NULL)
                            {
                              if((pstate_found_iterate->state_type ==
                                  STATE_TYPE_SHARE)
                                 && !memcmp(arg_OPEN4.owner.owner.owner_val,
                                            pstate_found_iterate->state_powner->so_owner_val,
                                            pstate_found_iterate->state_powner->so_owner_len)
                                 && !memcmp(pstate_found_iterate->state_data.share.
                                            oexcl_verifier,
                                            arg_OPEN4.openhow.openflag4_u.how.
                                            createhow4_u.createverf, NFS4_VERIFIER_SIZE))
                                {

                                  /* A former open EXCLUSIVE with same owner and verifier was found, resend it */

                                  // res_OPEN4.OPEN4res_u.resok4.attrset.bitmap4_len = 0 ; /* No attributes set */
                                  //if( ( res_OPEN4.OPEN4res_u.resok4.attrset.bitmap4_val = 
                                  //    (uint32_t *)Mem_Alloc( res_OPEN4.OPEN4res_u.resok4.attrset.bitmap4_len  * sizeof( uint32_t ) ) ) == NULL )
                                  // {
                                  //          res_OPEN4.status = NFS4ERR_SERVERFAULT ;
                                  //          return res_OPEN4.status ;
                                  //       }

                                  memset(&(res_OPEN4.OPEN4res_u.resok4.cinfo.after), 0,
                                         sizeof(changeid4));
                                  res_OPEN4.OPEN4res_u.resok4.cinfo.after =
                                      (changeid4) pentry_parent->internal_md.mod_time;
                                  res_OPEN4.OPEN4res_u.resok4.cinfo.atomic = TRUE;

                                  /* No delegation */
                                  res_OPEN4.OPEN4res_u.resok4.delegation.delegation_type =
                                      OPEN_DELEGATE_NONE;

                                  /* If server use OPEN_CONFIRM4, set the correct flag */
                                  P(powner->so_mutex);
                                  if(powner->so_owner.so_nfs4_owner.so_confirmed == FALSE)
                                    {
                                      if(nfs_param.nfsv4_param.use_open_confirm == TRUE)
                                        res_OPEN4.OPEN4res_u.resok4.rflags =
                                            OPEN4_RESULT_CONFIRM +
                                            OPEN4_RESULT_LOCKTYPE_POSIX;
                                      else
                                        res_OPEN4.OPEN4res_u.resok4.rflags =
                                            OPEN4_RESULT_LOCKTYPE_POSIX;
                                    }
                                  V(powner->so_mutex);

                                  /* Now produce the filehandle to this file */
                                  if((pnewfsal_handle =
                                      cache_inode_get_fsal_handle(pentry_lookup,
                                                                  &cache_status)) == NULL)
                                    {
                                      res_OPEN4.status = nfs4_Errno(cache_status);

                                      /* Save the response in the open owner */
                                      Copy_nfs4_state_req(powner, arg_OPEN4.seqid, op, data, resp, tag);

                                      return res_OPEN4.status;
                                    }

                                  /* Building a new fh */
                                  if(!nfs4_FSALToFhandle(&newfh4, pnewfsal_handle, data))
                                    {
                                      res_OPEN4.status = NFS4ERR_SERVERFAULT;

                                      /* Save the response in the open owner */
                                      Copy_nfs4_state_req(powner, arg_OPEN4.seqid, op, data, resp, tag);

                                      return res_OPEN4.status;
                                    }

                                  /* This new fh replaces the current FH */
                                  data->currentFH.nfs_fh4_len = newfh4.nfs_fh4_len;
                                  memcpy(data->currentFH.nfs_fh4_val, newfh4.nfs_fh4_val,
                                         newfh4.nfs_fh4_len);

                                  data->current_entry = pentry_lookup;
                                  data->current_filetype = REGULAR_FILE;

                                  /* regular exit */
                                  res_OPEN4.status = NFS4_OK;

                                  /* Handle stateid/seqid for success */
                                  update_stateid(pfile_state,
                                                 &res_OPEN4.OPEN4res_u.resok4.stateid,
                                                 data,
                                                 "LOCK");

                                  /* Save the response in the lock or open owner */
                                  Copy_nfs4_state_req(powner, arg_OPEN4.seqid, op, data, resp, tag);

                                  return res_OPEN4.status;
                                }

                            }
                          /* if( pstate_found_iterate != NULL ) */
                          pstate_previous_iterate = pstate_found_iterate;
                        }
                      while(pstate_found_iterate != NULL);
                    }
                }

              /* Managing GUARDED4 mode */
              if(cache_status != CACHE_INODE_SUCCESS)
                res_OPEN4.status = nfs4_Errno(cache_status);
              else
                res_OPEN4.status = NFS4ERR_EXIST;       /* File already exists */

              /* Save the response in the open owner */
              Copy_nfs4_state_req(powner, arg_OPEN4.seqid, op, data, resp, tag);

              return res_OPEN4.status;
            }

          /*  if( cache_status != CACHE_INODE_NOT_FOUND ), if file already exists basically */
          LogFullDebug(COMPONENT_NFS_V4_LOCK,
                       "    OPEN open.how = %d",
                       arg_OPEN4.openhow.openflag4_u.how.mode);

          /* Create the file, if we reach this point, it does not exist, we can create it */
          if((pentry_newfile = cache_inode_create(pentry_parent,
                                                  &filename,
                                                  REGULAR_FILE,
                                                  mode,
                                                  NULL,
                                                  &attr_newfile,
                                                  data->ht,
                                                  data->pclient,
                                                  data->pcontext, &cache_status)) == NULL)
            {
              /* If the file already exists, this is not an error if open mode is UNCHECKED */
              if(cache_status != CACHE_INODE_ENTRY_EXISTS)
                {
                  res_OPEN4.status = nfs4_Errno(cache_status);

                  /* Save the response in the open owner */
                  Copy_nfs4_state_req(powner, arg_OPEN4.seqid, op, data, resp, tag);

                  return res_OPEN4.status;
                }
              else
                {
                  /* If this point is reached, then the file already exists, cache_status == CACHE_INODE_ENTRY_EXISTS and pentry_newfile == NULL 
                     This probably means EXCLUSIVE4 mode is used and verifier matches. pentry_newfile is then set to pentry_lookup */
                  pentry_newfile = pentry_lookup;
                }
            }

          /* Prepare state management structure */
          candidate_type = STATE_TYPE_SHARE;
          candidate_data.share.share_deny = arg_OPEN4.share_deny;
          candidate_data.share.share_access = arg_OPEN4.share_access;
          candidate_data.share.lockheld = 0;

          /* If file is opened under mode EXCLUSIVE4, open verifier should be kept to detect non vicious double open */
          if(arg_OPEN4.openhow.openflag4_u.how.mode == EXCLUSIVE4)
            {
              strncpy(candidate_data.share.oexcl_verifier,
                      arg_OPEN4.openhow.openflag4_u.how.createhow4_u.createverf,
                      NFS4_VERIFIER_SIZE);
            }

          if(state_add(pentry_newfile,
                       candidate_type,
                       &candidate_data,
                       powner,
                       data->pclient,
                       data->pcontext,
                       &pfile_state, &state_status) != STATE_SUCCESS)
            {
              res_OPEN4.status = NFS4ERR_SHARE_DENIED;

              /* Save the response in the open owner */
              Copy_nfs4_state_req(powner, arg_OPEN4.seqid, op, data, resp, tag);

              return res_OPEN4.status;
            }

          cache_status = CACHE_INODE_SUCCESS;

          if(AttrProvided == TRUE)      /* Set the attribute if provided */
            {
              if((cache_status = cache_inode_setattr(pentry_newfile,
                                                     &sattr,
                                                     data->ht,
                                                     data->pclient,
                                                     data->pcontext,
                                                     &cache_status)) !=
                 CACHE_INODE_SUCCESS)
                {
                  res_OPEN4.status = nfs4_Errno(cache_status);

                   /* Save the response in the open owner */
                   Copy_nfs4_state_req(powner, arg_OPEN4.seqid, op, data, resp, tag);

                  return res_OPEN4.status;
                }

            }

          /* Set the openflags variable */
          if(arg_OPEN4.share_deny & OPEN4_SHARE_DENY_WRITE)
            openflags |= FSAL_O_RDONLY;
          if(arg_OPEN4.share_deny & OPEN4_SHARE_DENY_READ)
            openflags |= FSAL_O_WRONLY;
          if(arg_OPEN4.share_access & OPEN4_SHARE_ACCESS_WRITE)
            openflags = FSAL_O_RDWR;
          if(arg_OPEN4.share_access != 0)
            openflags = FSAL_O_RDWR;    /* @todo : BUGAZOMEU : Something better later */

          /* Open the file */
          if(cache_inode_open_by_name(pentry_parent,
                                      &filename,
                                      pentry_newfile,
                                      data->pclient,
                                      openflags,
                                      data->pcontext,
                                      &cache_status) != CACHE_INODE_SUCCESS)
            {
              res_OPEN4.status = NFS4ERR_ACCESS;

              /* Save the response in the open owner */
              Copy_nfs4_state_req(powner, arg_OPEN4.seqid, op, data, resp, tag);

              return res_OPEN4.status;
            }

          break;

        case OPEN4_NOCREATE:
          /* It was not a creation, but a regular open */
          /* The filehandle to the new file replaces the current filehandle */
          if(pentry_newfile == NULL)
            {
              if((pentry_newfile = cache_inode_lookup(pentry_parent,
                                                      &filename,
                                                      &attr_newfile,
                                                      data->ht,
                                                      data->pclient,
                                                      data->pcontext,
                                                      &cache_status)) == NULL)
                {
                  res_OPEN4.status = nfs4_Errno(cache_status);

                  /* Save the response in the open owner */
                  Copy_nfs4_state_req(powner, arg_OPEN4.seqid, op, data, resp, tag);

                  return res_OPEN4.status;
                }
            }

          /* OPEN4 is to be done on a file */
          if(pentry_newfile->internal_md.type != REGULAR_FILE)
            {
              if(pentry_newfile->internal_md.type == DIR_BEGINNING
                 || pentry_newfile->internal_md.type == DIR_CONTINUE)
                {
                  res_OPEN4.status = NFS4ERR_ISDIR;

                  /* Save the response in the open owner */
                  Copy_nfs4_state_req(powner, arg_OPEN4.seqid, op, data, resp, tag);

                  return res_OPEN4.status;
                }
              else if(pentry_newfile->internal_md.type == SYMBOLIC_LINK)
                {
                  res_OPEN4.status = NFS4ERR_SYMLINK;

                  /* Save the response in the open owner */
                  Copy_nfs4_state_req(powner, arg_OPEN4.seqid, op, data, resp, tag);

                  return res_OPEN4.status;
                }
              else
                {
                  res_OPEN4.status = NFS4ERR_INVAL;

                  /* Save the response in the open owner */
                  Copy_nfs4_state_req(powner, arg_OPEN4.seqid, op, data, resp, tag);

                  return res_OPEN4.status;
                }
            }

          /* If the file is opened for write, OPEN4 while deny share write access,
           * in this case, check caller has write access to the file */
          if(arg_OPEN4.share_deny & OPEN4_SHARE_DENY_WRITE)
            {
              if(cache_inode_access(pentry_newfile,
                                    write_access,
                                    data->ht,
                                    data->pclient,
                                    data->pcontext, &cache_status) != CACHE_INODE_SUCCESS)
                {
                  res_OPEN4.status = NFS4ERR_ACCESS;

                  /* Save the response in the open owner */
                  Copy_nfs4_state_req(powner, arg_OPEN4.seqid, op, data, resp, tag);

                  return res_OPEN4.status;
                }
              openflags = FSAL_O_WRONLY;
            }

          /* Same check on read: check for readability of a file before opening it for read */
          if(arg_OPEN4.share_access & OPEN4_SHARE_ACCESS_READ)
            {
              if(cache_inode_access(pentry_newfile,
                                    read_access,
                                    data->ht,
                                    data->pclient,
                                    data->pcontext, &cache_status) != CACHE_INODE_SUCCESS)
                {
                  res_OPEN4.status = NFS4ERR_ACCESS;

                  /* Save the response in the open owner */
                  Copy_nfs4_state_req(powner, arg_OPEN4.seqid, op, data, resp, tag);

                  return res_OPEN4.status;
                }
              openflags = FSAL_O_RDONLY;
            }

          /* Same check on write */
          if(arg_OPEN4.share_access & OPEN4_SHARE_ACCESS_WRITE)
            {
              if(cache_inode_access(pentry_newfile,
                                    write_access,
                                    data->ht,
                                    data->pclient,
                                    data->pcontext, &cache_status) != CACHE_INODE_SUCCESS)
                {
                  res_OPEN4.status = NFS4ERR_ACCESS;

                  /* Save the response in the open owner */
                  Copy_nfs4_state_req(powner, arg_OPEN4.seqid, op, data, resp, tag);

                  return res_OPEN4.status;
                }
              openflags = FSAL_O_RDWR;
            }
#ifdef WITH_MODE_0_CHECK
          /* If file mode is 000 then NFS4ERR_ACCESS should be returned for all cases and users */
          if(attr_newfile.mode == 0)
            {
              res_OPEN4.status = NFS4ERR_ACCESS;

              /* Save the response in the open owner */
              Copy_nfs4_state_req(powner, arg_OPEN4.seqid, op, data, resp, tag);

              return res_OPEN4.status;
            }
#endif

          /* Try to find if the same open_owner already has acquired a stateid for this file */
          pstate_found_iterate = NULL;
          pstate_previous_iterate = NULL;
          pfile_state = NULL;
          do
            {
              state_iterate(pentry_newfile,
                            &pstate_found_iterate,
                            pstate_previous_iterate,
                            data->pclient, data->pcontext, &state_status);

              if(state_status == STATE_STATE_ERROR)
                break;          /* Get out of the loop */

              if(state_status == STATE_INVALID_ARGUMENT)
                {
                  res_OPEN4.status = NFS4ERR_INVAL;

                  /* Save the response in the open owner */
                  Copy_nfs4_state_req(powner, arg_OPEN4.seqid, op, data, resp, tag);

                  return res_OPEN4.status;
                }

              cache_status = CACHE_INODE_SUCCESS;

              /* Check is open_owner is the same */
              if(pstate_found_iterate != NULL)
                {
                  if((pstate_found_iterate->state_type == STATE_TYPE_SHARE) &&
                     (pstate_found_iterate->state_powner->so_owner.so_nfs4_owner.so_clientid == arg_OPEN4.owner.clientid)
                     &&
                     ((pstate_found_iterate->state_powner->so_owner_len ==
                       arg_OPEN4.owner.owner.owner_len)
                      &&
                      (!memcmp
                       (arg_OPEN4.owner.owner.owner_val,
                        pstate_found_iterate->state_powner->so_owner_val,
                        pstate_found_iterate->state_powner->so_owner_len))))
                    {
                      /* We'll be re-using the found state */
                      pfile_state = pstate_found_iterate;
                    }
                  else
                    {
                      /* This is a different owner, check for possible conflicts */

                      if(memcmp(arg_OPEN4.owner.owner.owner_val,
                                pstate_found_iterate->state_powner->so_owner_val,
                                pstate_found_iterate->state_powner->so_owner_len))
                        {
                          if(pstate_found_iterate->state_type == STATE_TYPE_SHARE)
                            {
                              if((pstate_found_iterate->state_data.share.
                                  share_access & OPEN4_SHARE_ACCESS_WRITE)
                                 && (arg_OPEN4.share_deny & OPEN4_SHARE_DENY_WRITE))
                                {
                                  res_OPEN4.status = NFS4ERR_SHARE_DENIED;

                                  /* Save the response in the open owner */
                                  Copy_nfs4_state_req(powner, arg_OPEN4.seqid, op, data, resp, tag);

                                  return res_OPEN4.status;
                                }
                            }
                        }
                    }

                  /* In all cases opening in read access a read denied file or write access to a write denied file 
                   * should fail, even if the owner is the same, see discussion in 14.2.16 and 8.9 */
                  if(pstate_found_iterate->state_type == STATE_TYPE_SHARE)
                    {
                      /* deny read access on read denied file */
                      if((pstate_found_iterate->state_data.share.
                          share_deny & OPEN4_SHARE_DENY_READ)
                         && (arg_OPEN4.share_access & OPEN4_SHARE_ACCESS_READ))
                        {
                          res_OPEN4.status = NFS4ERR_SHARE_DENIED;

                          /* Save the response in the open owner */
                          Copy_nfs4_state_req(powner, arg_OPEN4.seqid, op, data, resp, tag);

                          return res_OPEN4.status;
                        }

                      /* deny write access on write denied file */
                      if((pstate_found_iterate->state_data.share.
                          share_deny & OPEN4_SHARE_DENY_WRITE)
                         && (arg_OPEN4.share_access & OPEN4_SHARE_ACCESS_WRITE))
                        {
                          res_OPEN4.status = NFS4ERR_SHARE_DENIED;

                          /* Save the response in the open owner */
                          Copy_nfs4_state_req(powner, arg_OPEN4.seqid, op, data, resp, tag);

                          return res_OPEN4.status;
                        }
                    }
                }
              /*  if( pstate_found_iterate != NULL ) */
              pstate_previous_iterate = pstate_found_iterate;
            }
          while(pstate_found_iterate != NULL);

          if(pfile_state == NULL)
            {
              /* Set the state for the related file */
              /* Prepare state management structure */
              candidate_type = STATE_TYPE_SHARE;
              candidate_data.share.share_deny = arg_OPEN4.share_deny;
              candidate_data.share.share_access = arg_OPEN4.share_access;
              candidate_data.share.lockheld = 0;

              if(state_add(pentry_newfile,
                           candidate_type,
                           &candidate_data,
                           powner,
                           data->pclient,
                           data->pcontext,
                           &pfile_state,
                           &state_status) != STATE_SUCCESS)
                {
                  res_OPEN4.status = NFS4ERR_SHARE_DENIED;

                  /* Save the response in the open owner */
                  Copy_nfs4_state_req(powner, arg_OPEN4.seqid, op, data, resp, tag);

                  return res_OPEN4.status;
                }
            }

          /* Open the file */
          if(cache_inode_open_by_name(pentry_parent,
                                      &filename,
                                      pentry_newfile,
                                      data->pclient,
                                      openflags,
                                      data->pcontext,
                                      &cache_status) != CACHE_INODE_SUCCESS)
            {
              res_OPEN4.status = NFS4ERR_ACCESS;

              /* Save the response in the open owner */
              Copy_nfs4_state_req(powner, arg_OPEN4.seqid, op, data, resp, tag);

              return res_OPEN4.status;
            }
          break;

        default:
          res_OPEN4.status = NFS4ERR_INVAL;

          /* Save the response in the open owner */
          Copy_nfs4_state_req(powner, arg_OPEN4.seqid, op, data, resp, tag);

          return res_OPEN4.status;
          break;
        }                       /* switch( arg_OPEN4.openhow.opentype ) */

      break;

    case CLAIM_PREVIOUS:
      // TODO FSF: doesn't this need to do something to re-establish state?
      powner = NULL;
      break;

    default:
      /* Invalid claim type */
      res_OPEN4.status = NFS4ERR_INVAL;

      /* Save the response in the open owner */
      Copy_nfs4_state_req(powner, arg_OPEN4.seqid, op, data, resp, tag);

      return res_OPEN4.status;
    }                           /*  switch(  arg_OPEN4.claim.claim ) */

  /* Now produce the filehandle to this file */
  if((pnewfsal_handle =
      cache_inode_get_fsal_handle(pentry_newfile, &cache_status)) == NULL)
    {
      res_OPEN4.status = nfs4_Errno(cache_status);

      /* Save the response in the open owner */
      Copy_nfs4_state_req(powner, arg_OPEN4.seqid, op, data, resp, tag);

      return res_OPEN4.status;
    }

  /* Building a new fh */
  if(!nfs4_FSALToFhandle(&newfh4, pnewfsal_handle, data))
    {
      res_OPEN4.status = NFS4ERR_SERVERFAULT;

      /* Save the response in the open owner */
      Copy_nfs4_state_req(powner, arg_OPEN4.seqid, op, data, resp, tag);

      return res_OPEN4.status;
    }

  /* This new fh replaces the current FH */
  data->currentFH.nfs_fh4_len = newfh4.nfs_fh4_len;
  memcpy(data->currentFH.nfs_fh4_val, newfh4.nfs_fh4_val, newfh4.nfs_fh4_len);

  data->current_entry = pentry_newfile;
  data->current_filetype = REGULAR_FILE;

  /* Status of parent directory after the operation */
  if((cache_status = cache_inode_getattr(pentry_parent,
                                         &attr_parent,
                                         data->ht,
                                         data->pclient,
                                         data->pcontext,
                                         &cache_status)) != CACHE_INODE_SUCCESS)
    {
      res_OPEN4.status = nfs4_Errno(cache_status);

      /* Save the response in the open owner */
      Copy_nfs4_state_req(powner, arg_OPEN4.seqid, op, data, resp, tag);

      return res_OPEN4.status;
    }

  res_OPEN4.OPEN4res_u.resok4.attrset.bitmap4_len = 2;
  if((res_OPEN4.OPEN4res_u.resok4.attrset.bitmap4_val =
      (uint32_t *) Mem_Alloc(res_OPEN4.OPEN4res_u.resok4.attrset.bitmap4_len *
                             sizeof(uint32_t))) == NULL)
    {
      res_OPEN4.status = NFS4ERR_SERVERFAULT;
      res_OPEN4.OPEN4res_u.resok4.attrset.bitmap4_len = 0;

      /* Save the response in the open owner */
      Copy_nfs4_state_req(powner, arg_OPEN4.seqid, op, data, resp, tag);

      return res_OPEN4.status;
    }
  res_OPEN4.OPEN4res_u.resok4.attrset.bitmap4_val[0] = 0;       /* No Attributes set */
  res_OPEN4.OPEN4res_u.resok4.attrset.bitmap4_val[1] = 0;       /* No Attributes set */

  if(arg_OPEN4.openhow.opentype == OPEN4_CREATE)
    {
      tmp_int = 2;
      tmp_attr[0] = FATTR4_SIZE;
      tmp_attr[1] = FATTR4_MODE;
      nfs4_list_to_bitmap4(&(res_OPEN4.OPEN4res_u.resok4.attrset), &tmp_int, tmp_attr);
      res_OPEN4.OPEN4res_u.resok4.attrset.bitmap4_len = 2;
    }

  res_OPEN4.OPEN4res_u.resok4.cinfo.after =
      (changeid4) pentry_parent->internal_md.mod_time;
  res_OPEN4.OPEN4res_u.resok4.cinfo.atomic = TRUE;

  /* No delegation */
  res_OPEN4.OPEN4res_u.resok4.delegation.delegation_type = OPEN_DELEGATE_NONE;

  /* If server use OPEN_CONFIRM4, set the correct flag */
  if(powner->so_owner.so_nfs4_owner.so_confirmed == FALSE)
    {
      if(nfs_param.nfsv4_param.use_open_confirm == TRUE)
        res_OPEN4.OPEN4res_u.resok4.rflags =
            OPEN4_RESULT_CONFIRM + OPEN4_RESULT_LOCKTYPE_POSIX;
      else
        res_OPEN4.OPEN4res_u.resok4.rflags = OPEN4_RESULT_LOCKTYPE_POSIX;
    }

  /* regular exit */
  res_OPEN4.status = NFS4_OK;

  /* Handle stateid/seqid for success */
  update_stateid(pfile_state,
                 &res_OPEN4.OPEN4res_u.resok4.stateid,
                 data,
                 "LOCK");

  /* Save the response in the lock or open owner */
  Copy_nfs4_state_req(powner, arg_OPEN4.seqid, op, data, resp, tag);
                
  return res_OPEN4.status;
}                               /* nfs4_op_open */

/**
 * nfs4_op_open_Free: frees what was allocared to handle nfs4_op_open.
 * 
 * Frees what was allocared to handle nfs4_op_open.
 *
 * @param resp  [INOUT]    Pointer to nfs4_op results
 *
 * @return nothing (void function )
 * 
 */
void nfs4_op_open_Free(OPEN4res * resp)
{
  if(resp->OPEN4res_u.resok4.attrset.bitmap4_val != NULL)
    Mem_Free(resp->OPEN4res_u.resok4.attrset.bitmap4_val);
  resp->OPEN4res_u.resok4.attrset.bitmap4_len = 0;
}                               /* nfs4_op_open_Free */

void nfs4_op_open_CopyRes(OPEN4res * resp_dst, OPEN4res * resp_src)
{
  if(resp_src->OPEN4res_u.resok4.attrset.bitmap4_val != NULL)
    {
      if((resp_dst->OPEN4res_u.resok4.attrset.bitmap4_val =
          (uint32_t *) Mem_Alloc(resp_dst->OPEN4res_u.resok4.attrset.bitmap4_len *
                                 sizeof(uint32_t))) == NULL)
        {
          resp_dst->OPEN4res_u.resok4.attrset.bitmap4_len = 0;
        }
    }
}
