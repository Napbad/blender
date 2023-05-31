/* SPDX-FileCopyrightText: 2001-2023 Blender Foundation
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "BKE_light_linking.h"

#include <string>

#include "MEM_guardedalloc.h"

#include "DNA_ID.h"
#include "DNA_collection_types.h"
#include "DNA_object_types.h"
#include "DNA_scene_types.h"

#include "BLI_assert.h"
#include "BLI_string.h"

#include "BKE_collection.h"
#include "BKE_layer.h"
#include "BKE_lib_id.h"
#include "BKE_report.h"

#include "BLT_translation.h"

#include "DEG_depsgraph.h"
#include "DEG_depsgraph_build.h"

void BKE_light_linking_free_if_empty(Object *object)
{
  if (object->light_linking->receiver_collection == nullptr &&
      object->light_linking->blocker_collection == nullptr)
  {
    MEM_SAFE_FREE(object->light_linking);
  }
}

Collection *BKE_light_linking_collection_get(const Object *object,
                                             const LightLinkingType link_type)
{
  if (!object->light_linking) {
    return nullptr;
  }

  switch (link_type) {
    case LIGHT_LINKING_RECEIVER:
      return object->light_linking->receiver_collection;
    case LIGHT_LINKING_BLOCKER:
      return object->light_linking->blocker_collection;
  }

  return nullptr;
}

static std::string get_default_collection_name(const Object *object,
                                               const LightLinkingType link_type)
{
  const char *format;

  switch (link_type) {
    case LIGHT_LINKING_RECEIVER:
      format = DATA_("Light Linking for %s");
      break;
    case LIGHT_LINKING_BLOCKER:
      format = DATA_("Shadow Linking for %s");
      break;
  }

  char name[MAX_ID_NAME];
  BLI_snprintf(name, sizeof(name), format, object->id.name + 2);

  return name;
}

Collection *BKE_light_linking_collection_new(struct Main *bmain,
                                             Object *object,
                                             const LightLinkingType link_type)
{
  const std::string collection_name = get_default_collection_name(object, link_type);

  Collection *new_collection = BKE_collection_add(bmain, nullptr, collection_name.c_str());

  BKE_light_linking_collection_assign(bmain, object, new_collection, link_type);

  return new_collection;
}

void BKE_light_linking_collection_assign_only(struct Object *object,
                                              struct Collection *new_collection,
                                              const LightLinkingType link_type)
{
  /* Remove user from old collection. */
  Collection *old_collection = BKE_light_linking_collection_get(object, link_type);
  if (old_collection) {
    id_us_min(&old_collection->id);
  }

  /* Allocate light linking on demand. */
  if (new_collection && !object->light_linking) {
    object->light_linking = MEM_cnew<LightLinking>(__func__);
  }

  if (object->light_linking) {
    /* Assign and increment user of new collection. */
    switch (link_type) {
      case LIGHT_LINKING_RECEIVER:
        object->light_linking->receiver_collection = new_collection;
        break;
      case LIGHT_LINKING_BLOCKER:
        object->light_linking->blocker_collection = new_collection;
        break;
      default:
        BLI_assert_unreachable();
        break;
    }

    if (new_collection) {
      id_us_plus(&new_collection->id);
    }

    BKE_light_linking_free_if_empty(object);
  }
}

void BKE_light_linking_collection_assign(struct Main *bmain,
                                         struct Object *object,
                                         struct Collection *new_collection,
                                         const LightLinkingType link_type)
{
  BKE_light_linking_collection_assign_only(object, new_collection, link_type);

  DEG_id_tag_update(&object->id, ID_RECALC_COPY_ON_WRITE | ID_RECALC_SHADING);
  DEG_relations_tag_update(bmain);
}

/* Add object to the light linking collection and return corresponding CollectionLightLinking
 * settings.
 *
 * If the object is already in the collection then the content of the collection is not modified,
 * and the existing light linking settings are returned. */
static CollectionLightLinking *light_linking_collection_add_object(Main *bmain,
                                                                   Collection *collection,
                                                                   Object *object)
{
  BKE_collection_object_add(bmain, collection, object);

  LISTBASE_FOREACH (CollectionObject *, collection_object, &collection->gobject) {
    if (collection_object->ob == object) {
      return &collection_object->light_linking;
    }
  }

  BLI_assert_msg(0, "Object was not found after added to the light linking collection");

  return nullptr;
}

/* Add child collection to the light linking collection and return corresponding
 * CollectionLightLinking settings.
 *
 * If the child collection is already in the collection then the content of the collection is
 * not modified, and the existing light linking settings are returned. */
static CollectionLightLinking *light_linking_collection_add_collection(Main *bmain,
                                                                       Collection *collection,
                                                                       Collection *child)
{
  BKE_collection_child_add(bmain, collection, child);

  LISTBASE_FOREACH (CollectionChild *, collection_child, &collection->children) {
    if (collection_child->collection == child) {
      return &collection_child->light_linking;
    }
  }

  BLI_assert_msg(0, "Collection was not found after added to the light linking collection");

  return nullptr;
}

void BKE_light_linking_add_receiver_to_collection(Main *bmain,
                                                  Collection *collection,
                                                  ID *receiver,
                                                  const eCollectionLightLinkingState link_state)
{
  const ID_Type id_type = GS(receiver->name);

  CollectionLightLinking *collection_light_linking = nullptr;

  if (id_type == ID_OB) {
    Object *object = reinterpret_cast<Object *>(receiver);
    if (!OB_TYPE_IS_GEOMETRY(object->type)) {
      return;
    }
    collection_light_linking = light_linking_collection_add_object(bmain, collection, object);
  }
  else if (id_type == ID_GR) {
    collection_light_linking = light_linking_collection_add_collection(
        bmain, collection, reinterpret_cast<Collection *>(receiver));
  }
  else {
    return;
  }

  if (!collection_light_linking) {
    return;
  }

  collection_light_linking->link_state = link_state;

  DEG_id_tag_update(&collection->id, ID_RECALC_HIERARCHY);
  DEG_id_tag_update(receiver, ID_RECALC_SHADING);

  DEG_relations_tag_update(bmain);
}

bool BKE_light_linking_unlink_id_from_collection(Main *bmain,
                                                 Collection *collection,
                                                 ID *id,
                                                 ReportList *reports)
{
  const ID_Type id_type = GS(id->name);

  if (id_type == ID_OB) {
    BKE_collection_object_remove(bmain, collection, reinterpret_cast<Object *>(id), false);
  }
  else if (id_type == ID_GR) {
    BKE_collection_child_remove(bmain, collection, reinterpret_cast<Collection *>(id));
  }
  else {
    BKE_reportf(reports,
                RPT_ERROR,
                "Cannot unlink unsupported '%s' from light linking collection '%s'",
                id->name + 2,
                collection->id.name + 2);
    return false;
  }

  DEG_id_tag_update(&collection->id, ID_RECALC_HIERARCHY);

  DEG_relations_tag_update(bmain);

  return true;
}

void BKE_light_linking_link_receiver_to_emitter(Main *bmain,
                                                Object *emitter,
                                                Object *receiver,
                                                const LightLinkingType link_type,
                                                const eCollectionLightLinkingState link_state)
{
  if (!OB_TYPE_IS_GEOMETRY(receiver->type)) {
    return;
  }

  Collection *collection = BKE_light_linking_collection_get(emitter, link_type);

  if (!collection) {
    collection = BKE_light_linking_collection_new(bmain, emitter, link_type);
  }

  BKE_light_linking_add_receiver_to_collection(bmain, collection, &receiver->id, link_state);
}

void BKE_light_linking_select_receivers_of_emitter(Scene *scene,
                                                   ViewLayer *view_layer,
                                                   Object *emitter,
                                                   const LightLinkingType link_type)
{
  Collection *collection = BKE_light_linking_collection_get(emitter, link_type);
  if (!collection) {
    return;
  }

  BKE_view_layer_synced_ensure(scene, view_layer);

  /* Deselect all currently selected objects in the view layer, but keep the emitter selected.
   * This is because the operation is called from the emitter being active, and it will be
   * confusing to deselect it but keep active. */
  LISTBASE_FOREACH (Base *, base, BKE_view_layer_object_bases_get(view_layer)) {
    if (base->object == emitter) {
      continue;
    }
    base->flag &= ~BASE_SELECTED;
  }

  /* Select objects which are reachable via the receiver collection hierarchy. */
  LISTBASE_FOREACH (CollectionObject *, cob, &collection->gobject) {
    Base *base = BKE_view_layer_base_find(view_layer, cob->ob);
    if (!base) {
      continue;
    }

    /* TODO(sergey): Check whether the object is configured to receive light. */

    base->flag |= BASE_SELECTED;
  }

  DEG_id_tag_update(&scene->id, ID_RECALC_SELECT);
}
