#pragma once

#include "structs.h"
#include <json-glib/json-glib.h>

void
signald_process_groupV2_obj(SignaldAccount *sa, JsonObject *obj);

void
signald_parse_groupV2_list(SignaldAccount *sa, JsonArray *groups);

GList *
signald_chat_info(PurpleConnection *pc);

GHashTable
*signald_chat_info_defaults(PurpleConnection *pc, const char *chat_name);

void
signald_join_chat(PurpleConnection *pc, GHashTable *data);

void
signald_set_chat_topic(PurpleConnection *pc, int id, const char *topic);

void
signald_request_group_list(SignaldAccount *sa);

PurpleConversation * signald_enter_group_chat(PurpleConnection *pc, const char *groupId, const char *title);

char *signald_get_chat_name(GHashTable *components);

PurpleRoomlist *signald_roomlist_get_list(PurpleConnection *pc);

GList *signald_blist_node_menu(PurpleBlistNode *node);

void signald_process_leave_group(SignaldAccount *sa, JsonObject *data);

void signald_chat_leave(PurpleConnection *pc, int id);

void signald_request_group_info(SignaldAccount *sa, const char *groupId);

void signald_update_participant_name(const char *uuid, JsonObject *obj);
