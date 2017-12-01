/*******************************************************************************
 *
 *                              Delta Chat Core
 *                      Copyright (C) 2017 Björn Petersen
 *                   Contact: r10s@b44t.com, http://b44t.com
 *
 * This program is free software: you can redistribute it and/or modify it under
 * the terms of the GNU General Public License as published by the Free Software
 * Foundation, either version 3 of the License, or (at your option) any later
 * version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 * FOR A PARTICULAR PURPOSE.  See the GNU General Public License for more
 * details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program.  If not, see http://www.gnu.org/licenses/ .
 *
 ******************************************************************************/


#include <assert.h>
#include <dirent.h>
#include <openssl/rand.h>
#include <libetpan/mmapstring.h>
#include <netpgp-extra.h>
#include "mrmailbox_internal.h"
#include "mrmimeparser.h"
#include "mrosnative.h"
#include "mrloginparam.h"
#include "mraheader.h"
#include "mrapeerstate.h"
#include "mrpgp.h"

static int s_imex_running = 0;
static int s_imex_do_exit = 1; /* the value 1 avoids mrmailbox_imex_cancel() from stopping already stopped threads */


/*******************************************************************************
 * Import
 ******************************************************************************/


static int import_self_keys(mrmailbox_t* mailbox, const char* dir_name)
{
	/* hint: even if we switch to import Autocrypt Setup files, we should leave the possibility to import
	plain ASC keys, at least keys without a password, if we do not want to implement a password entry function.
	Importing ASC keys is useful to use keys in Delta Chat used by any other non-Autocrypt-PGP implementation.

	Maybe we should make the "default" key handlong also a little bit smarter
	(currently, the last imported key is the standard key unless it contains the string "legacy" in its name) */

	int            imported_count = 0, locked = 0;
	DIR*           dir_handle = NULL;
	struct dirent* dir_entry = NULL;
	char*          suffix = NULL;
	char*          path_plus_name = NULL;
	mrkey_t*       private_key = mrkey_new();
	mrkey_t*       public_key = mrkey_new();
	sqlite3_stmt*  stmt = NULL;
	char*          self_addr = NULL;
	int            set_default = 0;

	if( mailbox==NULL || dir_name==NULL ) {
		goto cleanup;
	}

	if( (dir_handle=opendir(dir_name))==NULL ) {
		mrmailbox_log_error(mailbox, 0, "Import: Cannot open directory \"%s\".", dir_name);
		goto cleanup;
	}

	while( (dir_entry=readdir(dir_handle))!=NULL )
	{
		free(suffix);
		suffix = mr_get_filesuffix_lc(dir_entry->d_name);
		if( suffix==NULL || strcmp(suffix, "asc")!=0 ) {
			continue;
		}

		free(path_plus_name);
		path_plus_name = mr_mprintf("%s/%s", dir_name, dir_entry->d_name/* name without path; may also be `.` or `..` */);
		mrmailbox_log_info(mailbox, 0, "Checking: %s", path_plus_name);
		if( !mrkey_set_from_file(private_key, path_plus_name, mailbox) ) {
			mrmailbox_log_error(mailbox, 0, "Cannot read key from \"%s\".", path_plus_name);
			continue;
		}

		if( private_key->m_type!=MR_PRIVATE ) {
			continue; /* this is no error but quite normal as we always export the public keys together with the private ones */
		}

		if( !mrpgp_is_valid_key(mailbox, private_key) ) {
			mrmailbox_log_error(mailbox, 0, "\"%s\" is no valid key.", path_plus_name);
			continue;
		}

		if( !mrpgp_split_key(mailbox, private_key, public_key) ) {
			mrmailbox_log_error(mailbox, 0, "\"%s\" seems not to contain a private key.", path_plus_name);
			continue;
		}

		set_default = 1;
		if( strstr(dir_entry->d_name, "legacy")!=NULL ) {
			set_default = 0; /* a key with "legacy" in its name is not made default; this may result in a keychain with _no_ default, however, this is no problem, as this will create a default key later */
		}

		/* add keypair as default; before this, delete other keypairs with the same binary key and reset defaults */
		mrsqlite3_lock(mailbox->m_sql);
		locked = 1;

			stmt = mrsqlite3_prepare_v2_(mailbox->m_sql, "DELETE FROM keypairs WHERE public_key=? OR private_key=?;");
			sqlite3_bind_blob (stmt, 1, public_key->m_binary, public_key->m_bytes, SQLITE_STATIC);
			sqlite3_bind_blob (stmt, 2, private_key->m_binary, private_key->m_bytes, SQLITE_STATIC);
			sqlite3_step(stmt);
			sqlite3_finalize(stmt);
			stmt = NULL;

			if( set_default ) {
				mrsqlite3_execute__(mailbox->m_sql, "UPDATE keypairs SET is_default=0;"); /* if the new key should be the default key, all other should not */
			}

			free(self_addr);
			self_addr = mrsqlite3_get_config__(mailbox->m_sql, "configured_addr", NULL);
			if( !mrkey_save_self_keypair__(public_key, private_key, self_addr, set_default, mailbox->m_sql) ) {
				mrmailbox_log_error(mailbox, 0, "Cannot save keypair.");
				goto cleanup;
			}

			imported_count++;

		mrsqlite3_unlock(mailbox->m_sql);
		locked = 0;
	}

	if( imported_count == 0 ) {
		mrmailbox_log_error(mailbox, 0, "No private keys found in \"%s\".", dir_name);
		goto cleanup;
	}

cleanup:
	if( locked ) { mrsqlite3_unlock(mailbox->m_sql); }
	if( dir_handle ) { closedir(dir_handle); }
	free(suffix);
	free(path_plus_name);
	mrkey_unref(private_key);
	mrkey_unref(public_key);
	if( stmt ) { sqlite3_finalize(stmt); }
	free(self_addr);
	return imported_count;
}


/*******************************************************************************
 * Export keys
 ******************************************************************************/


static void export_key_to_asc_file(mrmailbox_t* mailbox, const char* dir, int id, const mrkey_t* key, int is_default)
{
	char* file_name;
	if( is_default ) {
		file_name = mr_mprintf("%s/%s-key-default.asc", dir, key->m_type==MR_PUBLIC? "public" : "private");
	}
	else {
		file_name = mr_mprintf("%s/%s-key-%i.asc", dir, key->m_type==MR_PUBLIC? "public" : "private", id);
	}
	mrmailbox_log_info(mailbox, 0, "Exporting key %s", file_name);
	mr_delete_file(file_name, mailbox);
	if( mrkey_render_asc_to_file(key, file_name, mailbox) ) {
		mailbox->m_cb(mailbox, MR_EVENT_IMEX_FILE_WRITTEN, (uintptr_t)file_name, 0);
		mrmailbox_log_error(mailbox, 0, "Cannot write key to %s", file_name);
	}
	free(file_name);
}


static int export_self_keys(mrmailbox_t* mailbox, const char* dir)
{
	int           success = 0;
	sqlite3_stmt* stmt = NULL;
	int           id = 0, is_default = 0;
	mrkey_t*      public_key = mrkey_new();
	mrkey_t*      private_key = mrkey_new();
	int           locked = 0;

	mrsqlite3_lock(mailbox->m_sql);
	locked = 1;

		if( (stmt=mrsqlite3_prepare_v2_(mailbox->m_sql, "SELECT id, public_key, private_key, is_default FROM keypairs;"))==NULL ) {
			goto cleanup;
		}

		while( sqlite3_step(stmt)==SQLITE_ROW ) {
			id = sqlite3_column_int(         stmt, 0  );
			mrkey_set_from_stmt(public_key,  stmt, 1, MR_PUBLIC);
			mrkey_set_from_stmt(private_key, stmt, 2, MR_PRIVATE);
			is_default = sqlite3_column_int( stmt, 3  );
			export_key_to_asc_file(mailbox, dir, id, public_key,  is_default);
			export_key_to_asc_file(mailbox, dir, id, private_key, is_default);
		}

		success = 1;

cleanup:
	if( locked ) { mrsqlite3_unlock(mailbox->m_sql); }
	if( stmt ) { sqlite3_finalize(stmt); }
	mrkey_unref(public_key);
	mrkey_unref(private_key);
	return success;
}



/*******************************************************************************
 * Export setup file
 ******************************************************************************/


/* a complete Autocrypt Setup Message looks like the following

To: me@mydomain.com
From: me@mydomain.com
Autocrypt-Setup-Message: v1
Content-type: multipart/mixed; boundary="==break1=="

	--==break1==
	Content-Type: text/plain

	This is the Autocrypt setup message.

	--==break1==
	Content-Type: application/autocrypt-key-backup
	Content-Disposition: attachment; filename="autocrypt-key-backup.html"

	<html>
	<body>
	<p>
		This is the Autocrypt setup file used to transfer keys between clients.
	</p>
	<pre>
	-----BEGIN PGP MESSAGE-----
	Version: BCPG v1.53
	Passphrase-Format: numeric9x4
	Passphrase-Begin: 12

	hQIMAxC7JraDy7DVAQ//SK1NltM+r6uRf2BJEg+rnpmiwfAEIiopU0LeOQ6ysmZ0
	CLlfUKAcryaxndj4sBsxLllXWzlNiFDHWw4OOUEZAZd8YRbOPfVq2I8+W4jO3Moe
	-----END PGP MESSAGE-----
	</pre>
	</body>
	</html>
	--==break1==--

The encrypted message part contains:

	-----BEGIN PGP PRIVATE KEY BLOCK-----
	Autocrypt-Prefer-Encrypt: mutual

	xcLYBFke7/8BCAD0TTmX9WJm9elc7/xrT4/lyzUDMLbuAuUqRINtCoUQPT2P3Snfx/jou1YcmjDgwT
	Ny9ddjyLcdSKL/aR6qQ1UBvlC5xtriU/7hZV6OZEmW2ckF7UgGd6ajE+UEjUwJg2+eKxGWFGuZ1P7a
	4Av1NXLayZDsYa91RC5hCsj+umLN2s+68ps5pzLP3NoK2zIFGoCRncgGI/pTAVmYDirhVoKh14hCh5
	.....
	-----END PGP PRIVATE KEY BLOCK-----

mrmailbox_render_keys_to_html() renders the part after the second `-==break1==` part in this example. */
int mrmailbox_render_keys_to_html(mrmailbox_t* mailbox, const char* passphrase, char** ret_msg)
{
	int                    success = 0, locked = 0;
	sqlite3_stmt*          stmt = NULL;
	char*                  self_addr = NULL;
	mrkey_t*               curr_private_key = mrkey_new();

	char                   passphrase_begin[8];
	uint8_t                salt[PGP_SALT_SIZE];
	#define                AES_KEY_LENGTH 16
	uint8_t                key[AES_KEY_LENGTH];

	pgp_output_t*          payload_output = NULL;
	pgp_memory_t*          payload_mem = NULL;

	pgp_output_t*          encr_output = NULL;
	pgp_memory_t*          encr_mem = NULL;
	char*                  encr_string = NULL;


	if( mailbox==NULL || passphrase==NULL || ret_msg==NULL
	 || strlen(passphrase)<2 || *ret_msg!=NULL || curr_private_key==NULL ) {
		goto cleanup;
	}

	strncpy(passphrase_begin, passphrase, 2);
	passphrase_begin[2] = 0;

	/* create the payload */

	{
		mrsqlite3_lock(mailbox->m_sql);
		locked = 1;

			self_addr = mrsqlite3_get_config__(mailbox->m_sql, "configured_addr", NULL);
			mrkey_load_self_private__(curr_private_key, self_addr, mailbox->m_sql);

			char* payload_key_asc = mrkey_render_asc(curr_private_key, mailbox->m_e2ee_enabled? "Autocrypt-Prefer-Encrypt: mutual\r\n" : NULL);
			if( payload_key_asc == NULL ) {
				goto cleanup;
			}

		mrsqlite3_unlock(mailbox->m_sql);
		locked = 0;

		//printf("\n~~~~~~~~~~~~~~~~~~~~SETUP-PAYLOAD~~~~~~~~~~~~~~~~~~~~\n%s~~~~~~~~~~~~~~~~~~~~/SETUP-PAYLOAD~~~~~~~~~~~~~~~~~~~~\n",key_asc); // DEBUG OUTPUT


		/* put the payload into a literal data packet which will be encrypted then, see RFC 4880, 5.7 :
		"When it has been decrypted, it contains other packets (usually a literal data packet or compressed data
		packet, but in theory other Symmetrically Encrypted Data packets or sequences of packets that form whole OpenPGP messages)" */

		pgp_setup_memory_write(&payload_output, &payload_mem, 128);
		pgp_write_litdata(payload_output, (const uint8_t*)payload_key_asc, strlen(payload_key_asc), PGP_LDT_BINARY);

		free(payload_key_asc);
	}


	/* create salt for the key */
	pgp_random(salt, PGP_SALT_SIZE);

	/* S2K */

	int s2k_spec = PGP_S2KS_SIMPLE; // 0=simple, 1=salted, 3=salted+iterated
	int s2k_iter_id = 0; // ~1000 iterations

	/* create key from setup-code using OpenPGP's salted+iterated S2K (String-to-key)
	(from netpgp/create.c) */

	{
		unsigned	done = 0;
		unsigned	i = 0;
		int         passphrase_len = strlen(passphrase);
		pgp_hash_t    hash;
		for (done = 0, i = 0; done < AES_KEY_LENGTH; i++) {
			unsigned 	hashsize;
			unsigned 	j;
			unsigned	needed;
			unsigned	size;
			uint8_t		zero = 0;
			uint8_t		*hashed;

			/* Hard-coded SHA1 for session key */
			pgp_hash_any(&hash, PGP_HASH_SHA1);
			hashsize = pgp_hash_size(PGP_HASH_SHA1);
			needed = AES_KEY_LENGTH - done;
			size = MR_MIN(needed, hashsize);
			if ((hashed = calloc(1, hashsize)) == NULL) {
				(void) fprintf(stderr, "write_seckey_body: bad alloc\n");
				return 0;
			}
			if (!hash.init(&hash)) {
				(void) fprintf(stderr, "write_seckey_body: bad alloc\n");
				free(hashed);
				return 0;
			}

			/* preload if iterating  */
			for (j = 0; j < i; j++) {
				/*
				 * Coverity shows a DEADCODE error on this
				 * line. This is expected since the hardcoded
				 * use of SHA1 and CAST5 means that it will
				 * not used. This will change however when
				 * other algorithms are supported.
				 */
				hash.add(&hash, &zero, 1);
			}

			if (s2k_spec & PGP_S2KS_SALTED) {
				hash.add(&hash, salt, PGP_SALT_SIZE);
			}

			hash.add(&hash, (uint8_t*)passphrase, (unsigned)passphrase_len);
			hash.finish(&hash, hashed);

			/*
			 * if more in hash than is needed by session key, use
			 * the leftmost octets
			 */
			(void) memcpy(&key[i * hashsize], hashed, (unsigned)size);
			done += (unsigned)size;
			free(hashed);
			if (done > AES_KEY_LENGTH) {
				(void) fprintf(stderr,
					"write_seckey_body: short add\n");
				return 0;
			}
		}
	}

	/* encrypt the payload using the key using AES-128 and put it into
	OpenPGP's "Symmetric-Key Encrypted Session Key" (Tag 3, https://tools.ietf.org/html/rfc4880#section-5.3 ) followed by
	OpenPGP's "Symmetrically Encrypted Data Packet" (Tag 9, https://tools.ietf.org/html/rfc4880#section-5.7 ) */

	pgp_setup_memory_write(&encr_output, &encr_mem, 128);
	pgp_writer_push_armor_msg(encr_output);

	/* Tag 3 */
	pgp_write_ptag     (encr_output, PGP_PTAG_CT_SK_SESSION_KEY);
	pgp_write_length   (encr_output, 1/*version*/ + 1/*algo*/ + /*S2K*/1+1+((s2k_spec&PGP_S2KS_SALTED)?PGP_SALT_SIZE:0)+((s2k_spec==PGP_S2KS_ITERATED_AND_SALTED)?1:0) );

	pgp_write_scalar   (encr_output, 4, 1);                  // 1 octet: version
	pgp_write_scalar   (encr_output, PGP_SA_AES_128, 1);     // 1 octet: symm. algo

	pgp_write_scalar   (encr_output, s2k_spec, 1);           // 1 octet
	pgp_write_scalar   (encr_output, PGP_HASH_SHA1, 1);      // 1 octet: S2 hash algo
	if( s2k_spec&PGP_S2KS_SALTED ) {
	  pgp_write        (encr_output, salt, PGP_SALT_SIZE);   // 8 octets: salt
	}
	if( s2k_spec==PGP_S2KS_ITERATED_AND_SALTED ) {
	  pgp_write_scalar (encr_output, s2k_iter_id, 1);  // 1 octets
	}

	for(int j=0; j<AES_KEY_LENGTH; j++) {
		printf("%02x", key[j]);
	}
		printf("\n----------------\n");

	/* Tag 18 */
	pgp_write_symm_enc_data((const uint8_t*)payload_mem->buf, payload_mem->length, PGP_SA_AES_128, key, encr_output);

	/* done with symmetric key block */
	pgp_writer_close(encr_output);
	encr_string = mr_null_terminate((const char*)encr_mem->buf, encr_mem->length);

	//printf("\n~~~~~~~~~~~~~~~~~~~~SYMMETRICALLY ENCRYPTED~~~~~~~~~~~~~~~~~~~~\n%s~~~~~~~~~~~~~~~~~~~~/SYMMETRICALLY ENCRYPTED~~~~~~~~~~~~~~~~~~~~\n",encr_string); // DEBUG OUTPUT


	/* add additional header to armored block */

	#define LINEEND "\r\n" /* use the same lineends as the PGP armored data */
	{
		char* replacement = mr_mprintf("-----BEGIN PGP MESSAGE-----" LINEEND
		                               "Passphrase-Format: numeric9x4" LINEEND
		                               "Passphrase-Begin: %s", passphrase_begin);
		mr_str_replace(&encr_string, "-----BEGIN PGP MESSAGE-----", replacement);
		free(replacement);
	}

	/* wrap HTML-commands with instructions around the encrypted payload */

	*ret_msg = mr_mprintf(
		"<!DOCTYPE html>" LINEEND
		"<html>" LINEEND
			"<head>" LINEEND
				"<title>Autocrypt setup file</title>" LINEEND
			"</head>" LINEEND
			"<body>" LINEEND
				"<h1>Autocrypt setup file</h1>" LINEEND
				"<p>This is the <a href=\"https://autocrypt.org\">Autocrypt</a> setup file used to transfer your secret key between clients.</p>" LINEEND
                "<p>To decrypt the key, you need the setup code that was shown to you when this file was created. Hint: The setup code starts with: <em>%s</em></p>" LINEEND
				"<h2>Encrypted key</h2>" LINEEND
				"<pre>" LINEEND
				"%s" LINEEND
				"</pre>" LINEEND
			"</body>" LINEEND
		"</html>" LINEEND,
		passphrase_begin,
		encr_string);

	success = 1;

cleanup:
	if( stmt ) { sqlite3_finalize(stmt); }
	if( locked ) { mrsqlite3_unlock(mailbox->m_sql); }

	if( payload_output ) { pgp_output_delete(payload_output); }
	if( payload_mem ) { pgp_memory_free(payload_mem); }

	if( encr_output ) { pgp_output_delete(encr_output); }
	if( encr_mem ) { pgp_memory_free(encr_mem); }
	free(encr_string);
	free(self_addr);
	return success;
}


static int export_setup_file(mrmailbox_t* mailbox, const char* dir, const char* setup_code)
{
	int           success = 0;
	char*         file_content = NULL;
	char*         file_name = mr_mprintf("%s/autocrypt-key-backup.html", dir);

	if( !mrmailbox_render_keys_to_html(mailbox, setup_code, &file_content) || file_content==NULL ) {
		mrmailbox_log_error(mailbox, 0, "Cannot generate Autocrypt setup file in %s", file_name);
		goto cleanup;
	}

	if( !mr_write_file(file_name, file_content, strlen(file_content), mailbox) ) {
		mrmailbox_log_error(mailbox, 0, "Cannot write keys to %s", file_name);
	}
	else {
		mailbox->m_cb(mailbox, MR_EVENT_IMEX_FILE_WRITTEN, (uintptr_t)file_name, 0);
	}

	success = 1;

cleanup:
	free(file_content);
	free(file_name);
	return success;
}



/*******************************************************************************
 * Export backup
 ******************************************************************************/


/* the FILE_PROGRESS macro calls the callback with the permille of files processed.
The macro avoids weird values of 0% or 100% while still working. */
#define FILE_PROGRESS \
	processed_files_count++; \
	int permille = (processed_files_count*1000)/total_files_count; \
	if( permille <  10 ) { permille =  10; } \
	if( permille > 990 ) { permille = 990; } \
	mailbox->m_cb(mailbox, MR_EVENT_IMEX_PROGRESS, permille, 0);


static int export_backup(mrmailbox_t* mailbox, const char* dir)
{
	int            success = 0, locked = 0, closed = 0;
	char*          dest_pathNfilename = NULL;
	mrsqlite3_t*   dest_sql = NULL;
	time_t         now = time(NULL);
	DIR*           dir_handle = NULL;
	struct dirent* dir_entry;
	int            prefix_len = strlen(MR_BAK_PREFIX);
	int            suffix_len = strlen(MR_BAK_SUFFIX);
	char*          curr_pathNfilename = NULL;
	void*          buf = NULL;
	size_t         buf_bytes = 0;
	sqlite3_stmt*  stmt = NULL;
	int            total_files_count = 0, processed_files_count = 0;
	int            delete_dest_file = 0;

	/* get a fine backup file name (the name includes the date so that multiple backup instances are possible) */
	{
		struct tm* timeinfo;
		char buffer[256];
		timeinfo = localtime(&now);
		strftime(buffer, 256, MR_BAK_PREFIX "-%Y-%m-%d." MR_BAK_SUFFIX, timeinfo);
		if( (dest_pathNfilename=mr_get_fine_pathNfilename(dir, buffer))==NULL ) {
			mrmailbox_log_error(mailbox, 0, "Cannot get backup file name.");
			goto cleanup;
		}
	}

	/* temporary lock and close the source (we just make a copy of the whole file, this is the fastest and easiest approach) */
	mrsqlite3_lock(mailbox->m_sql);
	locked = 1;
	mrsqlite3_close__(mailbox->m_sql);
	closed = 1;

	/* copy file to backup directory */
	mrmailbox_log_info(mailbox, 0, "Backup \"%s\" to \"%s\".", mailbox->m_dbfile, dest_pathNfilename);
	if( !mr_copy_file(mailbox->m_dbfile, dest_pathNfilename, mailbox) ) {
		goto cleanup; /* error already logged */
	}

	/* unlock and re-open the source and make it availabe again for the normal use */
	mrsqlite3_open__(mailbox->m_sql, mailbox->m_dbfile, 0);
	closed = 0;
	mrsqlite3_unlock(mailbox->m_sql);
	locked = 0;

	/* add all files as blobs to the database copy (this does not require the source to be locked, neigher the destination as it is used only here) */
	if( (dest_sql=mrsqlite3_new(mailbox/*for logging only*/))==NULL
	 || !mrsqlite3_open__(dest_sql, dest_pathNfilename, 0) ) {
		goto cleanup; /* error already logged */
	}

	if( !mrsqlite3_table_exists__(dest_sql, "backup_blobs") ) {
		if( !mrsqlite3_execute__(dest_sql, "CREATE TABLE backup_blobs (id INTEGER PRIMARY KEY, file_name, file_content);") ) {
			goto cleanup; /* error already logged */
		}
	}

	/* scan directory, pass 1: collect file info */
	total_files_count = 0;
	if( (dir_handle=opendir(mailbox->m_blobdir))==NULL ) {
		mrmailbox_log_error(mailbox, 0, "Backup: Cannot get info for blob-directory \"%s\".", mailbox->m_blobdir);
		goto cleanup;
	}

	while( (dir_entry=readdir(dir_handle))!=NULL ) {
		total_files_count++;
	}

	closedir(dir_handle);
	dir_handle = NULL;

	if( total_files_count>0 )
	{
		/* scan directory, pass 2: copy files */
		if( (dir_handle=opendir(mailbox->m_blobdir))==NULL ) {
			mrmailbox_log_error(mailbox, 0, "Backup: Cannot copy from blob-directory \"%s\".", mailbox->m_blobdir);
			goto cleanup;
		}

		stmt = mrsqlite3_prepare_v2_(dest_sql, "INSERT INTO backup_blobs (file_name, file_content) VALUES (?, ?);");
		while( (dir_entry=readdir(dir_handle))!=NULL )
		{
			if( s_imex_do_exit ) {
				delete_dest_file = 1;
				goto cleanup;
			}

			FILE_PROGRESS

			char* name = dir_entry->d_name; /* name without path; may also be `.` or `..` */
			int name_len = strlen(name);
			if( (name_len==1 && name[0]=='.')
			 || (name_len==2 && name[0]=='.' && name[1]=='.')
			 || (name_len > prefix_len && strncmp(name, MR_BAK_PREFIX, prefix_len)==0 && name_len > suffix_len && strncmp(&name[name_len-suffix_len-1], "." MR_BAK_SUFFIX, suffix_len)==0) ) {
				//mrmailbox_log_info(mailbox, 0, "Backup: Skipping \"%s\".", name);
				continue;
			}

			//mrmailbox_log_info(mailbox, 0, "Backup \"%s\".", name);
			free(curr_pathNfilename);
			curr_pathNfilename = mr_mprintf("%s/%s", mailbox->m_blobdir, name);
			free(buf);
			if( !mr_read_file(curr_pathNfilename, &buf, &buf_bytes, mailbox) || buf==NULL || buf_bytes<=0 ) {
				continue;
			}

			sqlite3_bind_text(stmt, 1, name, -1, SQLITE_STATIC);
			sqlite3_bind_blob(stmt, 2, buf, buf_bytes, SQLITE_STATIC);
			if( sqlite3_step(stmt)!=SQLITE_DONE ) {
				mrmailbox_log_error(mailbox, 0, "Disk full? Cannot add file \"%s\" to backup.", curr_pathNfilename);
				goto cleanup; /* this is not recoverable! writing to the sqlite database should work! */
			}
			sqlite3_reset(stmt);
		}
	}
	else
	{
		mrmailbox_log_info(mailbox, 0, "Backup: No files to copy.", mailbox->m_blobdir);
	}

	/* done - set some special config values (do this last to avoid importing crashed backups) */
	mrsqlite3_set_config_int__(dest_sql, "backup_time", now);
	mrsqlite3_set_config__    (dest_sql, "backup_for", mailbox->m_blobdir);

	mailbox->m_cb(mailbox, MR_EVENT_IMEX_FILE_WRITTEN, (uintptr_t)dest_pathNfilename, 0);
	success = 1;

cleanup:
	if( dir_handle ) { closedir(dir_handle); }
	if( closed ) { mrsqlite3_open__(mailbox->m_sql, mailbox->m_dbfile, 0); }
	if( locked ) { mrsqlite3_unlock(mailbox->m_sql); }

	if( stmt ) { sqlite3_finalize(stmt); }
	mrsqlite3_close__(dest_sql);
	mrsqlite3_unref(dest_sql);
	if( delete_dest_file ) { mr_delete_file(dest_pathNfilename, mailbox); }
	free(dest_pathNfilename);

	free(curr_pathNfilename);
	free(buf);
	return success;
}


/*******************************************************************************
 * Import backup
 ******************************************************************************/


static void ensure_no_slash(char* path)
{
	int path_len = strlen(path);
	if( path_len > 0 ) {
		if( path[path_len-1] == '/'
		 || path[path_len-1] == '\\' ) {
			path[path_len-1] = 0;
		}
	}
}


static int import_backup(mrmailbox_t* mailbox, const char* backup_to_import)
{
	/* command for testing eg.
	imex import-backup /home/bpetersen/temp/delta-chat-2017-11-14.bak
	*/

	int           success = 0;
	int           locked = 0;
	int           processed_files_count = 0, total_files_count = 0;
	sqlite3_stmt* stmt = NULL;
	char*         pathNfilename = NULL;
	char*         repl_from = NULL;
	char*         repl_to = NULL;

	mrmailbox_log_info(mailbox, 0, "Import \"%s\" to \"%s\".", backup_to_import, mailbox->m_dbfile);

	if( mrmailbox_is_configured(mailbox) ) {
		mrmailbox_log_error(mailbox, 0, "Cannot import backups to mailboxes in use.");
		goto cleanup;
	}

	/* close and delete the original file */
	mrmailbox_disconnect(mailbox);

	mrsqlite3_lock(mailbox->m_sql);
	locked = 1;

	if( mrsqlite3_is_open(mailbox->m_sql) ) {
		mrsqlite3_close__(mailbox->m_sql);
	}

	mr_delete_file(mailbox->m_dbfile, mailbox);

	if( mr_file_exist(mailbox->m_dbfile) ) {
		mrmailbox_log_error(mailbox, 0, "Cannot import backups: Cannot delete the old file.");
		goto cleanup;
	}

	/* copy the database file */
	if( !mr_copy_file(backup_to_import, mailbox->m_dbfile, mailbox) ) {
		goto cleanup; /* error already logged */
	}

	/* re-open copied database file */
	if( !mrsqlite3_open__(mailbox->m_sql, mailbox->m_dbfile, 0) ) {
		goto cleanup;
	}

	/* copy all blobs to files */
	stmt = mrsqlite3_prepare_v2_(mailbox->m_sql, "SELECT COUNT(*) FROM backup_blobs;");
	sqlite3_step(stmt);
	total_files_count = sqlite3_column_int(stmt, 0);
	sqlite3_finalize(stmt);
	stmt = NULL;

	stmt = mrsqlite3_prepare_v2_(mailbox->m_sql, "SELECT file_name, file_content FROM backup_blobs ORDER BY id;");
	while( sqlite3_step(stmt) == SQLITE_ROW )
	{
		if( s_imex_do_exit ) {
			goto cleanup;
		}

        FILE_PROGRESS

        const char* file_name    = (const char*)sqlite3_column_text (stmt, 0);
        int         file_bytes   = sqlite3_column_bytes(stmt, 1);
        const void* file_content = sqlite3_column_blob (stmt, 1);

        if( file_bytes > 0 && file_content ) {
			free(pathNfilename);
			pathNfilename = mr_mprintf("%s/%s", mailbox->m_blobdir, file_name);
			if( !mr_write_file(pathNfilename, file_content, file_bytes, mailbox) ) {
				mrmailbox_log_error(mailbox, 0, "Storage full? Cannot write file %s with %i bytes.", pathNfilename, file_bytes);
				goto cleanup; /* otherwise the user may believe the stuff is imported correctly, but there are files missing ... */
			}
		}
	}

	/* finalize/reset all statements - otherwise the table cannot be DROPped below */
	sqlite3_finalize(stmt);
	stmt = 0;
	mrsqlite3_reset_all_predefinitions(mailbox->m_sql);

	mrsqlite3_execute__(mailbox->m_sql, "DROP TABLE backup_blobs;");
	mrsqlite3_execute__(mailbox->m_sql, "VACUUM;");

	/* rewrite references to the blobs */
	repl_from = mrsqlite3_get_config__(mailbox->m_sql, "backup_for", NULL);
	if( repl_from && strlen(repl_from)>1 && mailbox->m_blobdir && strlen(mailbox->m_blobdir)>1 )
	{
		ensure_no_slash(repl_from);
		repl_to = safe_strdup(mailbox->m_blobdir);
		ensure_no_slash(repl_to);

		mrmailbox_log_info(mailbox, 0, "Rewriting paths from '%s' to '%s' ...", repl_from, repl_to);

		assert( 'f' == MRP_FILE );
		assert( 'i' == MRP_PROFILE_IMAGE );

		char* q3 = sqlite3_mprintf("UPDATE msgs SET param=replace(param, 'f=%q/', 'f=%q/');", repl_from, repl_to); /* cannot use mr_mprintf() because of "%q" */
			mrsqlite3_execute__(mailbox->m_sql, q3);
		sqlite3_free(q3);

		q3 = sqlite3_mprintf("UPDATE chats SET param=replace(param, 'i=%q/', 'i=%q/');", repl_from, repl_to);
			mrsqlite3_execute__(mailbox->m_sql, q3);
		sqlite3_free(q3);

		q3 = sqlite3_mprintf("UPDATE contacts SET param=replace(param, 'i=%q/', 'i=%q/');", repl_from, repl_to);
			mrsqlite3_execute__(mailbox->m_sql, q3);
		sqlite3_free(q3);
	}

	success = 1;

cleanup:
	free(pathNfilename);
	free(repl_from);
	free(repl_to);
	if( stmt )  { sqlite3_finalize(stmt); }
	if( locked ) { mrsqlite3_unlock(mailbox->m_sql); }
	return success;
}


/*******************************************************************************
 * Import/Export Thread and Main Interface
 ******************************************************************************/


/**
 * Import/export things.
 *
 * What to do is defined by the _what_ parameter which may be one of the following:
 *
 * - **MR_IMEX_EXPORT_BACKUP** (11) - Export a backup to the directory given as `param1`.
 *   The backup contains all contacts, chats, images and other data and device independent settings.
 *   The backup does not contain device dependent settings as ringtones or LED notification settings.
 *   The name of the backup is typically `delta-chat.<day>.bak`, if more than one backup is create on a day,
 *   the format is `delta-chat.<day>-<number>.bak`
 *
 * - **MR_IMEX_IMPORT_BACKUP** (12) - `param1` is the file (not: directory) to import. The file is normally
 *   created by MR_IMEX_EXPORT_BACKUP and detected by mrmailbox_imex_has_backup(). Importing a backup
 *   is only possible as long as the mailbox is not configured or used in another way.
 *
 * - **MR_IMEX_EXPORT_SELF_KEYS** (1) - Export all private keys and all public keys of the user to the
 *   directory given as `param1`.  The default key is written to the files `public-key-default.asc`
 *   and `private-key-default.asc`, if there are more keys, they are written to files as
 *   `public-key-<id>.asc` and `private-key-<id>.asc`
 *
 * - **MR_IMEX_IMPORT_SELF_KEYS** (2) - Import private keys found in the directory given as `param1`.
 *   The last imported key is made the default keys unless its name contains the string `legacy`.  Public keys are not imported.
 *
 * The function may take a long time until it finishes, so it might be a good idea to start it in a
 * separate thread. During its execution, the function sends out some events:
 *
 * - A number of #MR_EVENT_IMEX_PROGRESS events are sent and may be used to create
 *   a progress bar or stuff like that.
 *
 * - For each file written on export, the function sends #MR_EVENT_IMEX_FILE_WRITTEN
 *
 * Only one import-/export-progress can run at the same time.
 * To cancel an import-/export-progress, use mrmailbox_imex_cancel().
 *
 * @memberof mrmailbox_t
 *
 * @param mailbox Mailbox object as created by mrmailbox_new().
 * @param what One of the MR_IMEX_* constants.
 * @param param1 Meaning depends on the MR_IMEX_* constants. If this parameter is a directory, it should not end with
 *     a slash (otherwise you'll get double slashes when receiving #MR_EVENT_IMEX_FILE_WRITTEN). Set to NULL if not used.
 * @param param2 Meaning depends on the MR_IMEX_* constants. Set to NULL if not used.
 *
 * @return 1=success, 0=error or progress canceled.
 */
int mrmailbox_imex(mrmailbox_t* mailbox, int what, const char* param1, const char* param2)
{
	int success = 0;

	if( mailbox==NULL || mailbox->m_sql==NULL ) {
		return 0;
	}

	if( param1 == NULL ) {
		mrmailbox_log_error(mailbox, 0, "No Import/export dir/file given.");
		return 0;
	}

	if( s_imex_running || s_imex_do_exit==0 ) {
		mrmailbox_log_warning(mailbox, 0, "Already importing/exporting.");
		return 0;
	}

	s_imex_running = 1;
	s_imex_do_exit = 0;

	mrmailbox_log_info(mailbox, 0, "Import/export process started.");
	mailbox->m_cb(mailbox, MR_EVENT_IMEX_PROGRESS, 0, 0);

	if( !mrsqlite3_is_open(mailbox->m_sql) ) {
		mrmailbox_log_error(mailbox, 0, "Import/export: Database not opened.");
		goto cleanup;
	}

	if( what==MR_IMEX_EXPORT_SELF_KEYS || what==MR_IMEX_EXPORT_BACKUP ) {
		/* before we export anything, make sure the private key exists */
		if( !mrmailbox_ensure_secret_key_exists(mailbox) ) {
			mrmailbox_log_error(mailbox, 0, "Import/export: Cannot create private key or private key not available.");
			goto cleanup;
		}
		/* also make sure, the directory for exporting exists */
		mr_create_folder(param1, mailbox);
	}

	switch( what )
	{
		case MR_IMEX_EXPORT_SELF_KEYS:
			if( !export_self_keys(mailbox, param1) ) {
				goto cleanup;
			}
			break;

		case MR_IMEX_IMPORT_SELF_KEYS:
			if( !import_self_keys(mailbox, param1) ) {
				goto cleanup;
			}
			break;

		case MR_IMEX_EXPORT_BACKUP:
			if( !export_backup(mailbox, param1) ) {
				goto cleanup;
			}
			break;

		case MR_IMEX_IMPORT_BACKUP:
			if( !import_backup(mailbox, param1) ) {
				goto cleanup;
			}
			break;

		case MR_IMEX_EXPORT_SETUP_MESSAGE:
			if( !export_setup_file(mailbox, param1, param2) ) {
				goto cleanup;
			}
			break;

		default:
			goto cleanup;
	}

	success = 1;
	mailbox->m_cb(mailbox, MR_EVENT_IMEX_PROGRESS, 1000, 0);

cleanup:
	mrmailbox_log_info(mailbox, 0, "Import/export process ended.");
	s_imex_do_exit = 1; /* avoids mrmailbox_imex_cancel() to stop the thread */
	s_imex_running = 0;
	return success;
}


/**
 * Signal the import-/export-process to stop.
 *
 * After that, mrmailbox_imex_cancel() returns _without_ waiting
 * for mrmailbox_imex() to return.
 *
 * mrmailbox_imex() will return ASAP then, however, it may
 * still take a moment.  If in doubt, the caller may also decide the kill the
 * thread after a few seconds; eg. the process may hang in a
 * function not under the control of the core (eg. #MR_EVENT_HTTP_GET). Another
 * reason for mrmailbox_imex_cancel() not to wait is that otherwise it
 * would be GUI-blocking and should be started in another thread then; this
 * would make things even more complicated.
 *
 * @memberof mrmailbox_t
 *
 * @param mailbox The mailbox object.
 *
 * @return None
 */
void mrmailbox_imex_cancel(mrmailbox_t* mailbox)
{
	if( mailbox == NULL ) {
		return;
	}

	if( s_imex_running && s_imex_do_exit==0 )
	{
		mrmailbox_log_info(mailbox, 0, "Signaling the import-/export-process to stop ASAP.");
		s_imex_do_exit = 1;
	}
	else
	{
		mrmailbox_log_info(mailbox, 0, "No import-/export-process to stop.");
	}
}


/**
 * Check if there is a backup file.
 *
 * May only be used on fresh installations (eg. mrmailbox_is_configured() returns 0).
 *
 * @memberof mrmailbox_t
 *
 * @param mailbox Mailbox object as created by mrmailbox_new().
 * @param dir_name Directory to search backups in.
 *
 * @return String with the backup file, typically given to mrmailbox_imex(), returned strings must be free()'d.
 *     The function returns NULL if no backup was found.
 *
 * Example:
 * ```
 * char dir[] = "/dir/to/search/backups/in";
 *
 * void ask_user_for_credentials()
 * {
 *     // - ask the user for email and password
 *     // - save them using mrmailbox_set_config()
 * }
 *
 * int ask_user_whether_to_import()
 * {
 *     // - inform the user that we've found a backup
 *     // - ask if he want to import it
 *     // - return 1 to import, 0 to skip
 *     return 1;
 * }
 *
 * if( !mrmailbox_is_configured(mailbox) )
 * {
 *     char* file = NULL;
 *     if( (file=mrmailbox_imex_has_backup(mailbox, dir))!=NULL && ask_user_whether_to_import() )
 *     {
 *         mrmailbox_imex(mailbox, MR_IMEX_IMPORT_BACKUP, file, NULL);
 *         mrmailbox_connect(mailbox);
 *     }
 *     else
 *     {
 *         do {
 *             ask_user_for_credentials();
 *         }
 *         while( !mrmailbox_configure_and_connect(mailbox) )
 *     }
 *     free(file);
 * }
 * ```
 */
char* mrmailbox_imex_has_backup(mrmailbox_t* mailbox, const char* dir_name)
{
	char*          ret = NULL;
	time_t         ret_backup_time = 0;
	DIR*           dir_handle = NULL;
	struct dirent* dir_entry;
	int            prefix_len = strlen(MR_BAK_PREFIX);
	int            suffix_len = strlen(MR_BAK_SUFFIX);
	char*          curr_pathNfilename = NULL;
	mrsqlite3_t*   test_sql = NULL;

	if( mailbox == NULL ) {
		return NULL;
	}

	if( (dir_handle=opendir(dir_name))==NULL ) {
		mrmailbox_log_info(mailbox, 0, "Backup check: Cannot open directory \"%s\".", dir_name); /* this is not an error - eg. the directory may not exist or the user has not given us access to read data from the storage */
		goto cleanup;
	}

	while( (dir_entry=readdir(dir_handle))!=NULL ) {
		const char* name = dir_entry->d_name; /* name without path; may also be `.` or `..` */
		int name_len = strlen(name);
		if( name_len > prefix_len && strncmp(name, MR_BAK_PREFIX, prefix_len)==0
		 && name_len > suffix_len && strncmp(&name[name_len-suffix_len-1], "." MR_BAK_SUFFIX, suffix_len)==0 )
		{
			free(curr_pathNfilename);
			curr_pathNfilename = mr_mprintf("%s/%s", dir_name, name);

			mrsqlite3_unref(test_sql);
			if( (test_sql=mrsqlite3_new(mailbox/*for logging only*/))!=NULL
			 && mrsqlite3_open__(test_sql, curr_pathNfilename, MR_OPEN_READONLY) )
			{
				time_t curr_backup_time = mrsqlite3_get_config_int__(test_sql, "backup_time", 0); /* reading the backup time also checks if the database is readable and the table `config` exists */
				if( curr_backup_time > 0
				 && curr_backup_time > ret_backup_time/*use the newest if there are multiple backup*/ )
				{
					/* set return value to the tested database name */
					free(ret);
					ret = curr_pathNfilename;
					ret_backup_time = curr_backup_time;
					curr_pathNfilename = NULL;
				}
			}
		}
	}

cleanup:
	if( dir_handle ) { closedir(dir_handle); }
	free(curr_pathNfilename);
	mrsqlite3_unref(test_sql);
	return ret;
}


/**
 * Check if the user is authorized by the given password in some way.
 * This is to promt for the password eg. before exporting keys/backup.
 *
 * @memberof mrmailbox_t
 *
 * @param mailbox Mailbox object as created by mrmailbox_new().
 *
 * @param test_pw Password to check.
 *
 * @return 1=user is authorized, 0=user is not authorized.
 */
int mrmailbox_check_password(mrmailbox_t* mailbox, const char* test_pw)
{
	/* Check if the given password matches the configured mail_pw.
	This is to prompt the user before starting eg. an export; this is mainly to avoid doing people bad thinkgs if they have short access to the device.
	When we start supporting OAuth some day, we should think this over, maybe force the user to re-authenticate himself with the Android password. */
	mrloginparam_t* loginparam = mrloginparam_new();
	int             success = 0;

	if( mailbox==NULL ) {
		goto cleanup;
	}

	mrsqlite3_lock(mailbox->m_sql);

		mrloginparam_read__(loginparam, mailbox->m_sql, "configured_");

	mrsqlite3_unlock(mailbox->m_sql);

	if( (loginparam->m_mail_pw==NULL || loginparam->m_mail_pw[0]==0) && (test_pw==NULL || test_pw[0]==0) ) {
		/* both empty or unset */
		success = 1;
	}
	else if( loginparam->m_mail_pw==NULL || test_pw==NULL ) {
		/* one set, the other not */
		success = 0;
	}
	else if( strcmp(loginparam->m_mail_pw, test_pw)==0 ) {
		/* string-compared passwords are equal */
		success = 1;
	}

cleanup:
	mrloginparam_unref(loginparam);
	return success;
}


/**
 * Create random setup code.
 *
 * The created "Autocrypt Level 1" setup code has the following form:
 *
 * ```
 * 1234-1234-1234-
 * 1234-1234-1234-
 * 1234-1234-1234
 * ```
 *
 * Linebreaks and spaces are not added to the setup code, but the "-" are.
 * Should be given to  mrmailbox_imex() for encryption, should be wiped and free()'d after usage.
 *
 * @memberof mrmailbox_t
 *
 * @param mailbox Mailbox object as created by mrmailbox_new().
 *
 * @return Setup code, must be free()'d after usage.
 */
char* mrmailbox_create_setup_code(mrmailbox_t* mailbox)
{
	#define   CODE_ELEMS 9
	#define   BUF_BYTES  (CODE_ELEMS*sizeof(uint16_t))
	uint16_t  buf[CODE_ELEMS];
	int       i;

	if( !RAND_bytes((unsigned char*)buf, BUF_BYTES) ) {
		mrmailbox_log_warning(mailbox, 0, "Falling back to pseudo-number generation for the setup code.");
		RAND_pseudo_bytes((unsigned char*)buf, BUF_BYTES);
	}

	for( i = 0; i < CODE_ELEMS; i++ ) {
		buf[i] = buf[i] % 10000; /* force all blocks into the range 0..9999 */
	}

	return mr_mprintf("%04i-%04i-%04i-"
	                  "%04i-%04i-%04i-"
	                  "%04i-%04i-%04i",
		(int)buf[0], (int)buf[1], (int)buf[2],
		(int)buf[3], (int)buf[4], (int)buf[5],
		(int)buf[6], (int)buf[7], (int)buf[8]);
}
