/*
  $Id$

  Copyright (C) 1999-2004 IC & S  dbmail@ic-s.nl

  This program is free software; you can redistribute it and/or 
  modify it under the terms of the GNU General Public License 
  as published by the Free Software Foundation; either 
  version 2 of the License, or (at your option) any later 
  version.

  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program; if not, write to the Free Software
  Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
*/

/**
 * \file dbmail-message.c
 *
 * implements DbmailMessage object
 */

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <assert.h>

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "dbmail.h"
#include "dbmail-message.h"
#include "db.h"
#include "auth.h"
#include "misc.h"
#include "pipe.h"


extern db_param_t _db_params;
#define DBPFX _db_params.pfx

#define MESSAGE_MAX_LINE_SIZE 1024
/* for issuing queries to the backend */
char query[DEF_QUERYSIZE];

#define DBMAIL_TEMPMBOX "INBOX"
/*
 * _register_header
 *
 * register a message header in a ghashtable dictionary
 *
 */
static void _register_header(const char *header, const char *value, gpointer user_data);

/*
 * _retrieve
 *
 * retrieve message for id
 *
 */

static struct DbmailMessage * _retrieve(struct DbmailMessage *self, char *query_template);
static void _map_headers(struct DbmailMessage *self);
static void _set_content(struct DbmailMessage *self, const GString *content);
static void _set_content_from_stream(struct DbmailMessage *self, GMimeStream *stream, int type);

static int _message_insert(struct DbmailMessage *self, 
		u64_t user_idnr, 
		const char *mailbox, 
		const char *unique_id); 

struct DbmailMessage * dbmail_message_new(void)
{
	struct DbmailMessage *self = g_new0(struct DbmailMessage,1);
	if (! self) {
		trace(TRACE_ERROR, "%s,%s: memory error", __FILE__, __func__);
		return NULL;
	}
	g_mime_init(0);
	dbmail_message_set_class(self, DBMAIL_MESSAGE);
	return self;
}

struct DbmailMessage * dbmail_message_new_from_stream(FILE *instream, int streamtype) 
{
	GMimeStream *stream;
	struct DbmailMessage *message;
	
	assert(instream);
	message = dbmail_message_new();
	stream = g_mime_stream_fs_new(dup(fileno(instream)));
	message = dbmail_message_init_with_stream(message, stream, streamtype);
	return message;
}

int dbmail_message_set_class(struct DbmailMessage *self, int klass)
{
	switch (klass) {
		case DBMAIL_MESSAGE:
		case DBMAIL_MESSAGE_PART:
			self->klass = klass;
			break;
		default:
			return 1;
			break;
	}		
	return 0;
			
}
int dbmail_message_get_class(struct DbmailMessage *self)
{
	return self->klass;
}

struct DbmailMessage * dbmail_message_init_with_string(struct DbmailMessage *self, const GString *content)
{
	_set_content(self,content);
	/* If there's no From header assume it's a message-part and re-init */
	if (dbmail_message_get_class(self) == DBMAIL_MESSAGE) {
		if (! g_mime_message_get_header(GMIME_MESSAGE(self->content),"From")) {
			dbmail_message_set_class(self, DBMAIL_MESSAGE_PART);
			_set_content(self, content);
		}
	}
	_map_headers(self);
	
	return self;
}

struct DbmailMessage * dbmail_message_init_with_stream(struct DbmailMessage *self, GMimeStream *stream, int type)
{
	_set_content_from_stream(self,stream,type);
	_map_headers(self);
	return self;
}

static void _set_content(struct DbmailMessage *self, const GString *content)
{
	GMimeStream *stream = g_mime_stream_mem_new_with_buffer(content->str, content->len);
	_set_content_from_stream(self, stream, DBMAIL_STREAM_PIPE);
	g_object_unref(stream);
}
static void _set_content_from_stream(struct DbmailMessage *self, GMimeStream *stream, int type)
{
	/* 
	 * We convert all messages to crlf->lf for internal usage and
	 * db-insertion
	 */
	
	GMimeStream *ostream, *fstream, *bstream, *mstream;
	GMimeFilter *filter;
	GMimeParser *parser;
	char *buf = g_new0(char, MESSAGE_MAX_LINE_SIZE);

	/*
	 * buildup the memory stream buffer
	 * we will read from stream until either EOF or <dot><crlf> is encountered
	 * depending on the streamtype
	 */
        bstream = g_mime_stream_buffer_new(stream,GMIME_STREAM_BUFFER_CACHE_READ);
	mstream = g_mime_stream_mem_new();
        while (g_mime_stream_buffer_gets(bstream, buf, MESSAGE_MAX_LINE_SIZE)) {
                if ((type==DBMAIL_STREAM_LMTP) && (strncmp(buf,".\r\n",3)==0))
			break;
		g_mime_stream_write_string(mstream, buf);
	}
	g_mime_stream_reset(mstream);
	
	/* 
	 * filter mstream by decoding crlf and dot lines
	 * ostream will hold the decoded data
	 */
	ostream = g_mime_stream_mem_new();
	fstream = g_mime_stream_filter_new_with_stream(ostream);
	filter = g_mime_filter_crlf_new(GMIME_FILTER_CRLF_DECODE,GMIME_FILTER_CRLF_MODE_CRLF_DOTS);
	g_mime_stream_filter_add((GMimeStreamFilter *) fstream, filter);
	g_mime_stream_write_to_stream(mstream,fstream);
	g_mime_stream_reset(ostream);
	
	/*
	 * finally construct a message by parsing ostream
	 */
	parser = g_mime_parser_new_with_stream(ostream);
	switch (dbmail_message_get_class(self)) {
		case DBMAIL_MESSAGE:
			self->content = GMIME_OBJECT(g_mime_parser_construct_message(parser));
			break;
		case DBMAIL_MESSAGE_PART:
			self->content = GMIME_OBJECT(g_mime_parser_construct_part(parser));
			break;
	}
			
	
	g_object_unref(filter);
	g_object_unref(fstream);
	g_object_unref(ostream);
	g_object_unref(bstream);
	g_object_unref(mstream);
	g_object_unref(parser);
}

static void _map_headers(struct DbmailMessage *self) 
{
	GRelation *rel = g_relation_new(2);
	assert(self->content);
	g_relation_index(rel, 0, g_str_hash, g_str_equal);
	g_mime_header_foreach(GMIME_OBJECT(self->content)->headers, _register_header, rel);
	self->headers = rel;
}

static void _register_header(const char *header, const char *value, gpointer user_data)
{
	g_relation_insert((GRelation *)user_data, (gpointer)header, (gpointer)value);
}


size_t dbmail_message_get_rfcsize(struct DbmailMessage *self) 
{
	/*
	 * We convert all messages lf->crlf in-memory to determine
	 * the rfcsize
	 */

	size_t rfcsize;
	GMimeStream *ostream, *fstream;
	GMimeFilter *filter;
	
	ostream = g_mime_stream_mem_new();
	fstream = g_mime_stream_filter_new_with_stream(ostream);
	filter = g_mime_filter_crlf_new(GMIME_FILTER_CRLF_ENCODE,GMIME_FILTER_CRLF_MODE_CRLF_ONLY);
	
	g_mime_stream_filter_add((GMimeStreamFilter *) fstream, filter);
	g_mime_object_write_to_stream((GMimeObject *)self->content,fstream);
	
	rfcsize = g_mime_stream_length(ostream);
	
	g_object_unref(filter);
	g_object_unref(fstream);
	g_object_unref(ostream);

	return rfcsize;
}
/* dump message(parts) to char ptrs */
gchar * dbmail_message_to_string(struct DbmailMessage *self) 
{
	gchar *s;	
	GString *t;
	assert(self->content);
	
	t = g_string_new(g_mime_object_to_string(GMIME_OBJECT(self->content)));
	
	s = t->str;
	g_string_free(t,FALSE);
	return s;
}
gchar * dbmail_message_hdrs_to_string(struct DbmailMessage *self)
{
	char *s;
	GString *t;
	assert(self->headers);
	
	t = g_string_new(g_mime_object_get_headers((GMimeObject *)(self->content)));
	
	s = t->str;
	g_string_free(t,FALSE);
	return s;
}
gchar * dbmail_message_body_to_string(struct DbmailMessage *self)
{
	char *s;
	GString *t;
	assert(self->content);
	
	t = g_string_new(g_mime_object_to_string((GMimeObject *)(self->content)));
	t = g_string_erase(t,0,dbmail_message_get_hdrs_size(self));
	
	s = t->str;
	g_string_free(t,FALSE);
	return s;
}
/* 
 * we don't cache these values 
 * to allow changes in message content
 */
size_t dbmail_message_get_size(struct DbmailMessage *self)
{
	return strlen(dbmail_message_to_string(self));
}
size_t dbmail_message_get_hdrs_size(struct DbmailMessage *self)
{
	return strlen(dbmail_message_hdrs_to_string(self));
}
size_t dbmail_message_get_body_size(struct DbmailMessage *self)
{
	return strlen(dbmail_message_body_to_string(self));
}

void dbmail_message_free(struct DbmailMessage *self)
{
	if (self->headers)
		g_relation_destroy(self->headers);
	if (self->content)
		g_object_unref(self->content);
	self->headers=NULL;
	self->content=NULL;
	self->id=0;
	dm_free(self);
}


static struct DbmailMessage * _retrieve(struct DbmailMessage *self, char *query_template)
{
	
	int row = 0, rows = 0;
	GString *message = g_string_new("");
	
	assert(self->id != 0);
	
	snprintf(query, DEF_QUERYSIZE, query_template, DBPFX, DBPFX, self->id);

	if (db_query(query) == -1) {
		trace(TRACE_ERROR, "%s,%s: sql error", __FILE__, __func__);
		return NULL;
	}

	if (! (rows = db_num_rows())) {
		trace(TRACE_ERROR, "%s,%s: blk error", __FILE__, __func__);
		db_free_result();
		return NULL;	/* msg should have 1 block at least */
	}

	for (row=0; row < rows; row++)
		message = g_string_append(message, db_get_result(row, 0));
	db_free_result();
	
	return dbmail_message_init_with_string(self,message);
}

/*
 *
 * retrieve the header messageblk
 *
 * TODO: this call is yet unused in the code, but here for
 * forward compatibility's sake.
 *
 */
static void _fetch_head(struct DbmailMessage *self)
{
	char *query_template = 	"SELECT block.messageblk "
		"FROM %smessageblks block, %smessages msg "
		"WHERE block.physmessage_id = msg.physmessage_id "
		"AND block.is_header = 1"
		"AND msg.message_idnr = '%llu' "
		"ORDER BY block.messageblk_idnr";
	_retrieve(self, query_template);

}

/*
 *
 * retrieve the full message
 *
 */
static void _fetch_full(struct DbmailMessage *self) 
{
	char *query_template = "SELECT block.messageblk "
		"FROM %smessageblks block, %smessages msg "
		"WHERE block.physmessage_id = msg.physmessage_id "
		"AND msg.message_idnr = '%llu' "
		"ORDER BY block.messageblk_idnr";
	_retrieve(self, query_template);
}


struct DbmailMessage * dbmail_message_retrieve(struct DbmailMessage *self, u64_t id,int filter)
{
	self->id = id;
	
	switch (filter) {
		case DBMAIL_MESSAGE_FILTER_HEAD:
			_fetch_head(self);
			break;
		case DBMAIL_MESSAGE_FILTER_FULL:
			_fetch_full(self);
			break;
	}
	
	if (! self->content) {
		trace(TRACE_ERROR, 
				"%s,%s: retrieval failed for id [%llu]", 
				__FILE__, __func__, id
				);
		return NULL;
	}

	return self;
}


/**
 * store a temporary copy of a message.
 * \param 	self 
 * \param[out]	temp_message_idnr	message idnr of temporary message
 * \return 
 *     - -1 on error
 *     -  1 on success
 */
int dbmail_message_store_temp(struct DbmailMessage *self)
{
	u64_t user_idnr;
	u64_t messageblk_idnr;
	char unique_id[UID_SIZE];
	char *hdrs, *body;
	u64_t hdrs_size, body_size, rfcsize;
	
	switch (auth_user_exists(DBMAIL_DELIVERY_USERNAME, &user_idnr)) {
	case -1:
		trace(TRACE_ERROR,
		      "%s,%s: unable to find user_idnr for user " "[%s]\n",
		      __FILE__, __func__, DBMAIL_DELIVERY_USERNAME);
		return -1;
		break;
	case 0:
		trace(TRACE_ERROR,
		      "%s,%s: unable to find user_idnr for user "
		      "[%s]. Make sure this system user is in the database!\n",
		      __FILE__, __func__, DBMAIL_DELIVERY_USERNAME);
		return -1;
		break;
	}
	
	create_unique_id(unique_id, user_idnr);
	/* create a message record */
	if(_message_insert(self, user_idnr, DBMAIL_TEMPMBOX, unique_id) < 0)
		return -1;

	hdrs = dbmail_message_hdrs_to_string(self);
	body = dbmail_message_body_to_string(self);
	hdrs_size = (u64_t)dbmail_message_get_hdrs_size(self);
	body_size = (u64_t)dbmail_message_get_body_size(self);
	rfcsize = (u64_t)dbmail_message_get_rfcsize(self);
	
	if(db_insert_message_block(hdrs, hdrs_size, self->id, &messageblk_idnr,1) < 0)
		return -1;
	
	trace(TRACE_DEBUG, "%s,%s: allocating [%ld] bytes of memory "
	      "for readblock", __FILE__, __func__, READ_BLOCK_SIZE);
	
	/* store body in several blocks (if needed */
	if (store_message_in_blocks(body, body_size, self->id) < 0)
		return -1;

	if (db_update_message(self->id, unique_id, (hdrs_size + body_size), rfcsize) < 0) 
		return -1;

	g_free(hdrs);
	g_free(body);

	return 1;
}

int _message_insert(struct DbmailMessage *self, 
		u64_t user_idnr, 
		const char *mailbox, 
		const char *unique_id)
{
	u64_t mailboxid;
	u64_t physmessage_id;

	assert(unique_id);

	if (!mailbox)
		mailbox = dm_strdup("INBOX");

	if (db_find_create_mailbox(mailbox, user_idnr, &mailboxid) == -1)
		return -1;
	
	if (mailboxid == 0) {
		trace(TRACE_ERROR, "%s,%s: mailbox [%s] could not be found!", 
				__FILE__, __func__, mailbox);
		return -1;
	}

	/* insert a new physmessage entry */
	if (db_insert_physmessage(&physmessage_id) == -1) 
		return -1;

	/* now insert an entry into the messages table */
	snprintf(query, DEF_QUERYSIZE, "INSERT INTO "
		 "%smessages(mailbox_idnr, physmessage_id, unique_id,"
		 "recent_flag, status) "
		 "VALUES ('%llu', '%llu', '%s', '1', '%d')",
		 DBPFX, mailboxid, physmessage_id, unique_id,
		 MESSAGE_STATUS_INSERT);

	if (db_query(query) == -1) {
		trace(TRACE_STOP, "%s,%s: query failed", __FILE__, __func__);
		return -1;
	}

	self->id = db_insert_result("message_idnr");
	return 1;
}


