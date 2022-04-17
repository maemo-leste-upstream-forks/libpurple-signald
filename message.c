#include <sys/types.h>
#include <sys/stat.h>
#include <errno.h>

#include <MegaMimes.h>

#include "libsignald.h"

int
signald_get_external_attachment_settings(SignaldAccount *sa, const char **path, const char **url)
{
    *path = purple_account_get_string(sa->account, SIGNALD_ACCOUNT_OPT_EXT_ATTACHMENTS_DIR, "");
    *url = purple_account_get_string(sa->account, SIGNALD_ACCOUNT_OPT_EXT_ATTACHMENTS_URL, "");

    if (strlen(*path) == 0) {
        purple_debug_error(SIGNALD_PLUGIN_ID, "External attachments configured but no attachment path set.");

        return -1;
    }

    GFile *f = g_file_new_for_path(*path);
    GFileType type = g_file_query_file_type(f, G_FILE_QUERY_INFO_NONE, NULL);

    g_object_unref(f);

    if (type != G_FILE_TYPE_DIRECTORY) {
        purple_debug_error(SIGNALD_PLUGIN_ID, "External attachments path is not a valid directory: '%s'", *path);

        return -1;
    }

    if (strlen(*url) == 0) {
        purple_debug_error(SIGNALD_PLUGIN_ID, "External attachments configured but no attachment url set.");

        return -1;
    }

    return 0;
}

gchar *
signald_write_external_attachment(SignaldAccount *sa, const char *filename, const char *mimetype_remote)
{
    const char *path;
    const char *baseurl;
    gchar *url = NULL;

    if (signald_get_external_attachment_settings(sa, &path, &baseurl) != 0) {
        return NULL;
    }

    GFile *f = g_file_new_for_path(filename);
    GFileType type = g_file_query_file_type(f, G_FILE_QUERY_INFO_NONE, NULL);

    g_object_unref(f);

    if (type == G_FILE_TYPE_UNKNOWN) {
        purple_debug_error(SIGNALD_PLUGIN_ID, "Error accessing file (permission issue?)");

        return NULL;
    } else if (type != G_FILE_TYPE_REGULAR) {
        purple_debug_error(SIGNALD_PLUGIN_ID, "File is not a regular file... that's odd.");

        return NULL;
    }
    
    GFile *source = g_file_new_for_path(filename);
    char *basename = g_file_get_basename(source);

    gchar * ext = "unknown";
    char ** extensions = (char **)getMegaMimeExtensions(mimetype_remote);
    if (extensions && extensions[0]) {
        ext = extensions[0]+2;
    } else {
        purple_debug_error(SIGNALD_PLUGIN_ID, "Sender supplied mime-type %s. No extensions are known for this mime-type.", mimetype_remote);
    }
    gchar *destpath = g_strconcat(path, "/", basename, ".", ext, NULL);

    GFile *destination = g_file_new_for_path(destpath);
    GError *error;

    purple_debug_error(SIGNALD_PLUGIN_ID, "Copying attachment from '%s' to '%s'", filename, destpath);

    if (g_file_copy(source,
                    destination,
                    G_FILE_COPY_NONE,
                    NULL /* cancellable */,
                    NULL /* progress cb */,
                    NULL /* progress cb data */,
                    &error)) {

        url = g_strconcat(baseurl, "/", basename, ".", ext, NULL);
    } else {
        purple_debug_error(SIGNALD_PLUGIN_ID, "Error saving attachment to '%s': %s", destpath, error->message);

        g_error_free(error);
    }

    g_object_unref(source);
    g_object_unref(destination);

    g_free(destpath);
    
    freeMegaStringArray(extensions);

    return url;
}

void
signald_parse_attachment(SignaldAccount *sa, JsonObject *obj, GString *message)
{
    const char *type = json_object_get_string_member(obj, "contentType");
    const char *fn = json_object_get_string_member(obj, "storedFilename");

    if (purple_account_get_bool(sa->account, SIGNALD_ACCOUNT_OPT_EXT_ATTACHMENTS, FALSE)) {
        gchar *url = signald_write_external_attachment(sa, fn, type);

        if (url != NULL) {
            g_string_append_printf(message, "<a href=\"%s\">Attachment (type %s): %s</a><br/>", url, type, url);
            g_free(url);
        } else {
            g_string_append_printf(message, "An error occurred processing an attachment.  Enable debug logging for more information.");
        }

        return;
    }

    if (purple_strequal(type, "image/jpeg") || purple_strequal(type, "image/png")) {
        // TODO: forward "access denied" error to UI
        PurpleStoredImage *img = purple_imgstore_new_from_file(fn);
        size_t size = purple_imgstore_get_size(img);
        int img_id = purple_imgstore_add_with_id(g_memdup2(purple_imgstore_get_data(img), size), size, NULL);

        g_string_append_printf(message, "<IMG ID=\"%d\"/><br/>", img_id);
        g_string_append_printf(message, "<a href=\"file://%s\">Image (type: %s)</a><br/>", fn, type);
    } else {
        //TODO: Receive file using libpurple's file transfer API
        g_string_append_printf(message, "<a href=\"file://%s\">Attachment (type: %s)</a><br/>", fn, type);
    }

    purple_debug_info(SIGNALD_PLUGIN_ID, "Attachment: %s", message->str);
}

GString *
signald_prepare_attachments_message(SignaldAccount *sa, JsonObject *obj) {
    JsonArray *attachments = json_object_get_array_member(obj, "attachments");
    guint len = json_array_get_length(attachments);
    GString *attachments_message = g_string_sized_new(len * 100); // Preallocate buffer. Exact size doesn't matter. It grows automatically if it is too small

    for (guint i=0; i < len; i++) {
        signald_parse_attachment(sa, json_array_get_object_element(attachments, i), attachments_message);
    }

    return attachments_message;
}

const char *
signald_get_uuid_from_address(JsonObject *obj, const char *address_key)
{
    JsonObject *address = json_object_get_object_member(obj, address_key);
    if (address == NULL) {
        return NULL;
    } else {
        return (const char *)json_object_get_string_member(address, "uuid");
    }
}

static gboolean
signald_is_uuid(const gchar *identifier) {
    if (identifier) {
        return strlen(identifier) == 36;
    } else {
        return FALSE;
    }
}

static gboolean
signald_is_number(const gchar *identifier) {
    return identifier && identifier[0] == '+';
}

void
signald_set_recipient(SignaldAccount *sa, JsonObject *obj, gchar *recipient)
{
    g_return_if_fail(recipient);
    g_return_if_fail(obj);
    char * address_type = NULL;
    if (signald_is_number(recipient)) {
        address_type = "number";
    } else if (signald_is_uuid(recipient)) {
        address_type = "uuid";
    }
    g_return_if_fail(obj);
    JsonObject *address = json_object_new();
    // if contact was added manually and not yet migrated, the recipient might still be a number, not a UUID
    json_object_set_string_member(address, address_type, recipient);
    json_object_set_object_member(obj, "recipientAddress", address);
}

gboolean
signald_format_message(SignaldAccount *sa, SignaldMessage *msg, GString **target, gboolean *has_attachment)
{
    // handle attachments, creating appropriate message content (always allocates *target)
    *target = signald_prepare_attachments_message(sa, msg->data);

    if ((*target)->len > 0) {
        *has_attachment = TRUE;
    } else {
        *has_attachment = FALSE;
    }

    if (json_object_has_member(msg->data, "quote")) {

        JsonObject *quote = json_object_get_object_member(msg->data, "quote");

        JsonObject *author = json_object_get_object_member(quote, "author");
        const char *uuid = json_object_get_string_member(author, "uuid");
        PurpleBuddy *buddy = purple_find_buddy(sa->account, uuid);
        const char *alias = purple_buddy_get_alias(buddy);

        const char *text = json_object_get_string_member(quote, "text");
        gchar **lines = g_strsplit(text, "\n", 0);

        if (lines[0] != NULL) {
            g_string_append_printf(*target, "> %s: %s\n", alias, lines [0]);
            for (int i = 1; lines[i] != NULL; i++) {
                g_string_append_printf(*target, "> %s\n", lines[i]);
            }
        }

        g_strfreev(lines);
    }
    
    if (json_object_has_member(msg->data, "reaction")) {
        JsonObject *reaction = json_object_get_object_member(msg->data, "reaction");
        const char *emoji = json_object_get_string_member(reaction, "emoji");
        const gboolean remove = json_object_get_boolean_member(reaction, "remove");
        const time_t targetSentTimestamp = json_object_get_int_member(reaction, "targetSentTimestamp") / 1000;
        struct tm *tm = localtime(&targetSentTimestamp);
        if (remove) {
            g_string_printf(*target, "removed their %s reaction.", emoji);
        } else {
            g_string_printf(*target, "reacted with %s (to message from %s).", emoji, purple_date_format_long(tm));
        }
    }

    // append actual message text
    g_string_append(*target, json_object_get_string_member(msg->data, "body"));

    return (*target)->len > 0; // message not empty
}

gboolean
signald_parse_message(SignaldAccount *sa, JsonObject *obj, SignaldMessage *msg)
{
    if (msg == NULL) {
        return FALSE;
    }

    JsonObject *syncMessage = json_object_get_object_member(obj, "sync_message");

    // Signal's integer timestamps are in milliseconds
    // timestamp, timestampISO and dataMessage.timestamp seem to always be the same value (message sent time)
    // serverTimestamp is when the server received the message

    msg->envelope = obj;
    msg->timestamp = json_object_get_int_member(obj, "timestamp") / 1000;
    msg->is_sync_message = (syncMessage != NULL);

    if (syncMessage != NULL) {
        JsonObject *sent = json_object_get_object_member(syncMessage, "sent");

        if (sent == NULL) {
            return FALSE;
        }

        msg->conversation_name = (char *)signald_get_uuid_from_address(sent, "destination");
        msg->data = json_object_get_object_member(sent, "message");
     } else {
        JsonObject *source = json_object_get_object_member(obj, "source");
        msg->conversation_name = (char *)json_object_get_string_member(source, "uuid");
        msg->data = json_object_get_object_member(obj, "data_message");
     }

    if (msg->data == NULL) {
        return FALSE;
    }

    if (msg->conversation_name == NULL) {
        msg->conversation_name = SIGNALD_UNKNOWN_SOURCE_NUMBER;
    }

    if (json_object_has_member(msg->data, "group")) {
        msg->type = SIGNALD_MESSAGE_TYPE_GROUP;
        // TODO: remove support for obsolete V1 groups completly
    } else if (json_object_has_member(msg->data, "groupV2")) {
        msg->type = SIGNALD_MESSAGE_TYPE_GROUPV2;
    } else {
        msg->type = SIGNALD_MESSAGE_TYPE_DIRECT;
    }

    return TRUE;
}

int
signald_send_message(SignaldAccount *sa, SignaldMessageType type, gchar *recipient, const char *message)
{
    JsonObject *data = json_object_new();

    json_object_set_string_member(data, "type", "send");
    json_object_set_string_member(data, "username", purple_account_get_username(sa->account));
    //json_object_set_string_member(data, "account", sa->uuid); // alternative to supplying the username, mutually exclusive

    if (type == SIGNALD_MESSAGE_TYPE_DIRECT) {
        signald_set_recipient(sa, data, recipient);
    } else if (type == SIGNALD_MESSAGE_TYPE_GROUP) {
        json_object_set_string_member(data, "recipientGroupId", recipient);
    } else {
        return -1;
    }

    // Search for embedded images and attach them to the message. Remove the <img> tags.
    JsonArray *attachments = json_array_new();
    GString *msg = g_string_new(""); // this shall hold the actual message body (without the <img> tags)
    GData *attribs;
    const char *start, *end, *last;

    last = message;

    /* for each valid IMG tag... */
    while (last && *last && purple_markup_find_tag("img", last, &start, &end, &attribs))
    {
        PurpleStoredImage *image = NULL;
        const char *id;

        if (start - last) {
            g_string_append_len(msg, last, start - last);
        }

        id = g_datalist_get_data(&attribs, "id");

        /* ... if it refers to a valid purple image ... */
        if (id && (image = purple_imgstore_find_by_id(atoi(id)))) {
            unsigned long size = purple_imgstore_get_size(image);
            gconstpointer imgdata = purple_imgstore_get_data(image);
            gchar *tmp_fn = NULL;
            GError *error = NULL;
            //TODO: This is not very secure. But attachment handling should be reworked in signald to allow sending them in the same stream as the message
            //Signal requires the filename to end with a known image extension. However it does not care if the extension matches the image format.
            //contentType is ignored completely.
            //https://gitlab.com/thefinn93/signald/issues/11

            gint file = g_file_open_tmp("XXXXXX.png", &tmp_fn, &error);
            if (file == -1) {
                purple_debug_error(SIGNALD_PLUGIN_ID, "Error: %s\n", error->message);
                // TODO: show this error to the user
            } else {
                close(file); // will be re-opened by g_file_set_contents
                error = NULL;
                if (!g_file_set_contents(tmp_fn, imgdata, size, &error)) {
                    purple_debug_error(SIGNALD_PLUGIN_ID, "Error: %s\n", error->message);
                } else {
                    chmod(tmp_fn, 0644);
                    JsonObject *attachment = json_object_new();
                    json_object_set_string_member(attachment, "filename", tmp_fn);
//                    json_object_set_string_member(attachment, "caption", "Caption");
//                    json_object_set_string_member(attachment, "contentType", "image/png");
//                    json_object_set_int_member(attachment, "width", 150);
//                    json_object_set_int_member(attachment, "height", 150);
                    json_array_add_object_element(attachments, attachment);
                }
                g_free(tmp_fn);
                //TODO: Check for memory leaks
                //TODO: Delete file when response from signald is received
            }
        }
        /* If the tag is invalid, skip it, thus no else here */

        g_datalist_clear(&attribs);

        /* continue from the end of the tag */
        last = end + 1;
    }

    /* append any remaining message data */
    if (last && *last) {
        g_string_append(msg, last);
    }

    json_object_set_array_member(data, "attachments", attachments);

    char *plain = purple_unescape_html(msg->str);

    json_object_set_string_member(data, "messageBody", plain);

    // TODO: check if json_object_set_string_member manages copies of the data it is given (else these would be read from free'd memory)

    int ret = !purple_account_get_bool(sa->account, SIGNALD_OPTION_WAIT_SEND_ACKNOWLEDEMENT, FALSE);

    if (!signald_send_json(sa, data)) {
        ret = -errno;
    }

    if (ret == 0) {
        // free last message just in case it still lingers in memory
        g_free(sa->last_message);
        // store message for later echo
        sa->last_message = g_string_free(msg, FALSE);
        // store this as the currently active conversation
        sa->last_conversation = purple_find_conversation_with_account(PURPLE_CONV_TYPE_ANY, recipient, sa->account);
        if (sa->last_conversation == NULL) {
            // no appropriate conversation was found. maybe it is a group?
            SignaldGroup *group = (SignaldGroup *)g_hash_table_lookup(sa->groups, recipient);
            if (group != NULL) {
                sa->last_conversation = group->conversation;
            }
        }
    } else if (ret > 0) {
        // do not store message, display immediately
        g_string_free(msg, TRUE);
    }

    g_free(plain);
    json_object_unref(data);

    return ret;
}

static void
signald_send_check_result(JsonArray* results, guint i, JsonNode* result_node, gpointer user_data) {
    int * devices_count_ptr = (int *)user_data;
    JsonObject * result = json_node_get_object(result_node);
    JsonObject * success = json_object_get_object_member(result, "success");
    if (success) {
        JsonArray * devices = json_object_get_array_member(success, "devices");
        if (devices) {
            *devices_count_ptr += json_array_get_length(devices);
        }
    }
}

void
signald_send_acknowledged(SignaldAccount *sa,  JsonObject *data) {
    time_t timestamp = json_object_get_int_member(data, "timestamp") / 1000;
    int devices_count = 0;
    JsonArray * results = json_object_get_array_member(data, "results");
    if (results) {
        if (json_array_get_length(results) == 0) {
            // when sending message to self, the results array is empty
            // TODO: check if recipient actually was sa->uuid
            devices_count = 1;
        } else {
            json_array_foreach_element(results, signald_send_check_result, &devices_count);
        }
    }
    if (sa->last_conversation && sa->uuid && sa->last_message) {
        if (devices_count > 0) {
            PurpleMessageFlags flags = PURPLE_MESSAGE_SEND | PURPLE_MESSAGE_REMOTE_SEND | PURPLE_MESSAGE_DELAYED;
            purple_conversation_write(sa->last_conversation, sa->uuid, sa->last_message, flags, timestamp);
            g_free(sa->last_message);
            sa->last_message = NULL;
        } else {
            // form purple_conv_present_error()
            purple_conversation_write(sa->last_conversation, NULL, "Message was not delivered to any devices.", PURPLE_MESSAGE_ERROR, time(NULL));
        }
    } else if (devices_count == 0) {
        purple_debug_error(SIGNALD_PLUGIN_ID, "A message was not delivered to any devices.\n");
    }
}
