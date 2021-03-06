/* =========================================================================
    zsync_node - node on ZeroSync network

   -------------------------------------------------------------------------
   Copyright (c) 2013 Kevin Sapper, Bernhard Finger
   Copyright other contributors as noted in the AUTHORS file.
   
   This file is part of ZeroSync, see http://zerosync.org.
   
   This is free software; you can redistribute it and/or modify it under
   the terms of the GNU Lesser General Public License as published by the
   Free Software Foundation; either version 3 of the License, or (at your
   option) any later version.
   This software is distributed in the hope that it will be useful, but
   WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTA-
   BILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU Lesser General
   Public License for more details.
   
   You should have received a copy of the GNU Lesser General Public License
   along with this program. If not, see http://www.gnu.org/licenses/.
   =========================================================================
*/

/*
@header
    ZeroSync node

    A node is one instance on the ZeroSync network
@discuss
    LOGGER into zyre_event.
    LOG message to LOG group on whisper and shout
@end
*/

#include "zsync_classes.h"

#define UUID_FILE ".zsync_uuid"
#define PEER_STATES_FILE ".zsync_peer_states"

struct _zsync_node_t {
    zctx_t *ctx;
    zyre_t *zyre;               // Zyre
    void *zsync_pipe;           // Pipe to communicate with agent
    void *credit_pipe;          // Pipe to credit manager
    void *file_pipe;            // Pipe to file manager
    zuuid_t *own_uuid;          // uuid of this node
    zlist_t *peers;
    zhash_t *zyre_peers;        // mapping of zyre id to zsync peers
    bool terminated;
};

static zsync_node_t *
zsync_node_new ()
{
    int rc;
    zsync_node_t *self = (zsync_node_t *) zmalloc (sizeof (zsync_node_t));
   
    self->ctx = zctx_new ();
    assert (self->ctx);
    self->zyre = zyre_new (self->ctx);  
    assert (self->zyre);
    
    // Obtain permanent UUID
    self->own_uuid = zuuid_new ();
    if (zsys_file_exists (UUID_FILE)) {
        // Read uuid from file
        zfile_t *uuid_file = zfile_new (".", UUID_FILE);
        int rc = zfile_input (uuid_file);    // open file for reading        
        assert (rc == 0); 

        zchunk_t *uuid_chunk = zfile_read (uuid_file, 16, 0);
        assert (zchunk_size (uuid_chunk) == 16);    // make sure read succeeded
        zuuid_set (self->own_uuid, zchunk_data (uuid_chunk));
        zfile_destroy (&uuid_file);
    } else {
        // Write uuid to file
        zfile_t *uuid_file = zfile_new (".", UUID_FILE);
        rc = zfile_output (uuid_file); // open file for writing
        assert (rc == 0);
        zchunk_t *uuid_bin = zchunk_new ( zuuid_data (self->own_uuid), 16);
        rc = zfile_write (uuid_file, uuid_bin, 0);
        assert (rc == 0);
        zfile_destroy (&uuid_file);
    }
    
    // Obtain peers and states
    self->peers = zlist_new ();
    if (zsys_file_exists (PEER_STATES_FILE)) {
        zhash_t *peer_states = zhash_new ();
        int rc = zhash_load (peer_states, PEER_STATES_FILE);
        assert (rc == 0);
        zlist_t *uuids = zhash_keys (peer_states);
        char *uuid = zlist_first (uuids);
        while (uuid) {
            char * state_str = zhash_lookup (peer_states, uuid);
            uint64_t state;
            sscanf (state_str, "%"SCNd64, &state);
            zlist_append (self->peers, zsync_peer_new (uuid, state)); 
            uuid = zlist_next (uuids);
        }
    }
    
    self->zyre_peers = zhash_new ();
    self->terminated = false;
    return self;
}

static void
zsync_node_destroy (zsync_node_t **self_p)
{
    assert (self_p);

    if (*self_p) {
        zsync_node_t *self = *self_p;
        
        zuuid_destroy (&self->own_uuid);
      
        // TODO destroy all zsync_peers
        zlist_destroy (&self->peers);
        zhash_destroy (&self->zyre_peers);
        zyre_destroy (&self->zyre);

        free (self);
        self_p = NULL;
    }
}

static zsync_peer_t *
zsync_node_peers_lookup (zsync_node_t *self, char *uuid)
{
    assert (self);
    zsync_peer_t *peer = zlist_first (self->peers);
    while (peer) {
        if (streq (zsync_peer_uuid (peer), uuid)) {
            return peer;
        }
        peer = zlist_next (self->peers);
    }
    return NULL;
}

static int
zsync_node_save_peers (zsync_node_t *self)
{
    assert (self);
    int rc;
    
    // Obtain peer state key-values
    zhash_t *peer_states = zhash_new ();
        
    zsync_peer_t *peer = zlist_first (self->peers);
    while (peer) {
        char *uuid = zsync_peer_uuid (peer);
        char state[20];
        sprintf (state, "%"PRId64, zsync_peer_state (peer));
        zhash_insert (peer_states, uuid, state); 
        peer = zlist_next (self->peers);
    }
    rc = zhash_save (peer_states, PEER_STATES_FILE);
    return rc;
}

static char *
zsync_node_zyre_uuid (zsync_node_t *self, char *sender)
{
    assert (self);
    assert (sender);

    zlist_t *keys = zhash_keys (self->zyre_peers);
    char *key = zlist_first (keys);
    while (key) {
        zsync_peer_t *peer = zhash_lookup (self->zyre_peers, key);
        if (peer && streq (sender, zsync_peer_uuid (peer))) {
           return key;
        }
        key = zlist_next (keys);
    }
    return NULL;
}
    
static void
zsync_node_recv_from_zyre (zsync_node_t *self)
{
    zsync_peer_t *sender;
    char *zyre_sender;
    zuuid_t *sender_uuid;
    zmsg_t *zyre_in, *zyre_out, *fm_msg;
    zlist_t *fpaths, *fmetadata;
    
    zyre_event_t *event = zyre_event_recv (self->zyre);
    zyre_sender = zyre_event_sender (event); // get tmp uuid

    switch (zyre_event_type (event)) {
        case ZYRE_EVENT_ENTER:
            printf("[ND] ZS_ENTER: %s\n", zyre_sender);
            zhash_insert (self->zyre_peers, zyre_sender, NULL);
            break;        
        case ZYRE_EVENT_JOIN:
            printf ("[ND] ZS_JOIN: %s\n", zyre_sender);
            //  Obtain own current state
            zsync_msg_send_req_state (self->zsync_pipe);
            zsync_msg_t *msg_state = zsync_msg_recv (self->zsync_pipe);
            assert (zsync_msg_id (msg_state) == ZSYNC_MSG_RES_STATE);
            uint64_t state = zsync_msg_state (msg_state); 
            //  Send GREET message
            zyre_out = zmsg_new ();
            zs_msg_pack_greet (zyre_out, zuuid_data (self->own_uuid), state);
            zyre_whisper (self->zyre, zyre_sender, &zyre_out);
            break;
        case ZYRE_EVENT_LEAVE:
            break;
        case ZYRE_EVENT_EXIT:
            /*
            printf("[ND] ZS_EXIT %s left the house!\n", zyre_sender);
            sender = zhash_lookup (self->zyre_peers, zyre_sender);
            if (sender) {
                // Reset Managers
                zmsg_t *reset_msg = zmsg_new ();
                zmsg_addstr (reset_msg, zsync_peer_uuid (sender));
                zmsg_addstr (reset_msg, "ABORT");
                zmsg_send (&reset_msg, self->file_pipe);
                reset_msg = zmsg_new ();
                zmsg_addstr (reset_msg, zsync_peer_uuid (sender));
                zmsg_addstr (reset_msg, "ABORT");
                zmsg_send (&reset_msg, self->credit_pipe);
                // Remove Peer from active list
                zhash_delete (self->zyre_peers, zyre_sender);
            }*/
            break;
        case ZYRE_EVENT_WHISPER:
        case ZYRE_EVENT_SHOUT:
            printf ("[ND] ZS_WHISPER: %s\n", zyre_sender);
            sender = zhash_lookup (self->zyre_peers, zyre_sender);
            zyre_in = zyre_event_msg (event);
            zs_msg_t *msg = zs_msg_unpack (zyre_in);
            switch (zs_msg_get_cmd (msg)) {
                case ZS_CMD_GREET:
                    // Get perm uuid
                    sender_uuid = zuuid_new ();
                    zuuid_set (sender_uuid, zs_msg_uuid (msg));
                    sender = zsync_node_peers_lookup (self, zuuid_str (sender_uuid));
                    if (!sender) {
                        sender = zsync_peer_new (zuuid_str (sender_uuid), 0x0);
                        zlist_append (self->peers, sender);
                    } 
                    assert (sender);
                    zhash_update (self->zyre_peers, zyre_sender, sender);
                    zsync_peer_set_zyre_state (sender, ZYRE_EVENT_JOIN);
                    // Get current state for sender
                    uint64_t remote_current_state = zs_msg_get_state (msg);
                    printf ("[ND] current state: %"PRId64"\n", remote_current_state);
                    // Lookup last known state
                    uint64_t last_state_local = zsync_peer_state (sender);
                    printf ("[ND] last known state: %"PRId64"\n", zsync_peer_state (sender));
                    // Send LAST_STATE if differs 
                    if (remote_current_state >= last_state_local) {
                        zmsg_t *lmsg = zmsg_new ();
                        zs_msg_pack_last_state (lmsg, last_state_local);
                        zyre_whisper (self->zyre, zyre_sender, &lmsg);
                    }  
                    break;
                case ZS_CMD_LAST_STATE:
                    assert (sender);
                    zyre_out = zmsg_new ();
                    //  Gets updates from client
                    uint64_t last_state_remote = zs_msg_get_state (msg);
                    zsync_msg_send_req_update (self->zsync_pipe, last_state_remote);
                    zsync_msg_t *msg_upd = zsync_msg_recv (self->zsync_pipe);
                    assert (zsync_msg_id (msg_upd) == ZSYNC_MSG_UPDATE);
                    //  Send UPDATE
                    zyre_out = zsync_msg_update_msg (msg_upd);
                    zyre_whisper (self->zyre, zyre_sender, &zyre_out);
                    break;
                case ZS_CMD_UPDATE:
                    printf ("[ND] UPDATE\n");
                    assert (sender);
                    uint64_t state = zs_msg_get_state (msg);
                    zsync_peer_set_state (sender, state); 
                    zsync_node_save_peers (self);

                    fmetadata = zs_msg_get_fmetadata (msg);
                    zmsg_t *zsync_msg = zmsg_new ();
                    zs_msg_pack_update (zsync_msg, zs_msg_get_state (msg), fmetadata);
                    
                    zsync_msg_send_update (self->zsync_pipe, zsync_peer_uuid (sender), zsync_msg);
                    break;
                case ZS_CMD_REQUEST_FILES:
                    printf ("[ND] REQUEST FILES\n");
                    fpaths = zs_msg_fpaths (msg);
                    zmsg_t *fm_msg = zmsg_new ();
                    zmsg_addstr (fm_msg, zsync_peer_uuid (sender));
                    zmsg_addstr (fm_msg, "REQUEST");
                    char *fpath = zs_msg_fpaths_first (msg);
                    while (fpath) {
                        zmsg_addstr (fm_msg, fpath);
                        printf("[ND] %s\n", fpath);
                        fpath = zs_msg_fpaths_next (msg);
                    }
                    zmsg_send (&fm_msg, self->file_pipe);
                    break;
                case ZS_CMD_GIVE_CREDIT:
                    printf("[ND] GIVE CREDIT\n");
                    fm_msg = zmsg_new ();
                    zmsg_addstr (fm_msg, zsync_peer_uuid (sender));
                    zmsg_addstr (fm_msg, "CREDIT");
                    zmsg_addstrf (fm_msg, "%"PRId64, zs_msg_get_credit (msg));
                    zmsg_send (&fm_msg, self->file_pipe);
                    break;
                case ZS_CMD_SEND_CHUNK:
                    printf("[ND] SEND_CHUNK (RCV)\n");
                    // Send receival to credit manager
                    zframe_t *zframe = zs_msg_get_chunk (msg);
                    uint64_t chunk_size = zframe_size (zframe);
                    zsync_credit_msg_send_update (self->credit_pipe, zsync_peer_uuid (sender), chunk_size);
                    // Pass chunk to client
                    byte *data = zframe_data (zframe);
                    zchunk_t *chunk = zchunk_new (data, chunk_size);
                    char *path = zs_msg_get_file_path (msg);
                    uint64_t seq = zs_msg_get_sequence (msg);
                    uint64_t off = zs_msg_get_offset (msg);
                    zsync_msg_send_chunk (self->zsync_pipe, chunk, path, seq, off);
                    break;
                case ZS_CMD_ABORT:
                    // TODO abort protocol managed file transfer
                    printf("[ND] ABORT\n");
                    break;
                default:
                    assert (false);
                    break;
            }
            
            zs_msg_destroy (&msg);
            break;
        default:
            printf("[ND] Error command not found\n");
            break;
    }
    zyre_event_destroy (&event);
}

void
zsync_node_recv_from_agent (zsync_node_t *self)
{
    assert (self);
    zsync_msg_t *msg = zsync_msg_recv (self->zsync_pipe);
    switch (zsync_msg_id (msg)) {
        case ZSYNC_MSG_REQ_FILES: {
            char *receiver = zsync_msg_receiver (msg);
            char *zyre_uuid = zsync_node_zyre_uuid (self, receiver);
            if (zyre_uuid) {
                uint64_t size = zsync_msg_size (msg);
                printf("[ND] Recv Agent WHISPER REQUEST %s ; %s\n", zyre_uuid, receiver);
                zmsg_t *zyre_out = zmsg_new ();
                zs_msg_pack_request_files (zyre_out, zsync_msg_files (msg));
                zyre_whisper (self->zyre, zyre_uuid, &zyre_out);
                zsync_credit_msg_send_request (self->credit_pipe, zyre_uuid, size);
            }
            break;
        }
        case ZSYNC_MSG_UPDATE:
            printf("[ND] Recv Agent SHOUT UPDATE\n");
            zmsg_t *zyre_out = zsync_msg_update_msg (msg);
            zyre_shout (self->zyre, "ZSYNC", &zyre_out);
            break;                     
        case ZSYNC_MSG_TERMINATE:
            zyre_stop (self->zyre);
            // terminate file transfer manager
            zsync_ftm_msg_send_terminate (self->file_pipe);
            // terminate credit manager
            zsync_credit_msg_send_terminate (self->credit_pipe);
            // receive termination confirmation
            msg = zsync_msg_recv (self->file_pipe);
            zsync_msg_destroy (&msg);
            printf("OK ft\n");
            msg = zsync_msg_recv (self->credit_pipe);
            zsync_msg_destroy (&msg);
            printf("OK cm\n");

            // send shutdown confirmation to agent
            zsync_msg_send_terminate (self->zsync_pipe);
            self->terminated = true;
            break;            
    }
}


void
zsync_node_engine (void *args, zctx_t *ctx, void *pipe)
{
    int rc;    
    zsync_node_t *self = zsync_node_new ();
    self->ctx = ctx;
    self->zyre = zyre_new (ctx);
    self->zsync_pipe = pipe;
    
    // Join group
    rc = zyre_join (self->zyre, "ZSYNC");
    assert (rc == 0);

    // Give time to interconnect
    zclock_sleep (250);

    zpoller_t *poller = zpoller_new (zyre_socket (self->zyre), self->zsync_pipe, NULL);
    
    // Create thread for file management
    self->file_pipe = zthread_fork (self->ctx, zsync_ftmanager_engine, NULL);
    zpoller_add (poller, self->file_pipe);
    // Create thread for credit management
    self->credit_pipe = zthread_fork (self->ctx, zsync_credit_manager_engine, NULL);
    zpoller_add (poller, self->credit_pipe);

    // Start receiving messages
    printf("[ND] started\n");
    while (!zpoller_terminated (poller)) {
        void *which = zpoller_wait (poller, -1);
        
        if (which == zyre_socket (self->zyre)) {
            zsync_node_recv_from_zyre (self);
        } 
        else
        if (which == self->zsync_pipe) {
            printf("[ND] Recv Agent\n");
            zsync_node_recv_from_agent (self);
        }
        else
        if (which == self->file_pipe) {
            printf("[ND] Recv FT Manager\n");
            zsync_ftm_msg_t *msg = zsync_ftm_msg_recv (self->file_pipe);
            char *receiver = zsync_ftm_msg_receiver (msg);
            char *zyre_uuid = zsync_node_zyre_uuid (self, receiver);
            if (zyre_uuid) {
                char *path = zsync_ftm_msg_path (msg);
                uint64_t sequence = zsync_ftm_msg_sequence (msg);
                uint64_t chunk_size = zsync_ftm_msg_chunk_size (msg);
                uint64_t offset = zsync_ftm_msg_offset (msg);
                zsync_msg_send_req_chunk (pipe, path, chunk_size, offset);
                zsync_msg_t *zsmsg = zsync_msg_recv (pipe);
                zchunk_t *chunk = zsync_msg_chunk (zsmsg);
                zframe_t *frame = zframe_new (zchunk_data (chunk), zchunk_size (chunk));

                zmsg_t *zmsg = zmsg_new ();
                zs_msg_pack_chunk (zmsg, sequence, path, offset, frame);
                
                zyre_whisper (self->zyre, zyre_uuid, &zmsg);
                zsync_ftm_msg_destroy (&msg);
                zsync_msg_destroy (&zsmsg);
            }
        }
        else
        if (which == self->credit_pipe) {
            printf("[ND] Recv Credit Manager\n");
            zsync_credit_msg_t *cmsg = zsync_credit_msg_recv (self->credit_pipe);
            char *receiver = zsync_credit_msg_receiver (cmsg);
            char *zyre_uuid = zsync_node_zyre_uuid (self, receiver);
            if (zyre_uuid) {
                zmsg_t *credit_msg = zsync_credit_msg_credit (cmsg);
                assert (rc == 0);
                zyre_whisper (self->zyre, zyre_uuid, &credit_msg);
            }
            zsync_credit_msg_destroy (&cmsg);
        }
        if (self->terminated) {
            break;
        }
    }
    zpoller_destroy (&poller);
    zsync_node_destroy (&self);
    printf("[ND] stopped\n");
}

int
zsync_node_test () 
{
    printf (" * zsync_node: ");
    printf("OK\n");
}

