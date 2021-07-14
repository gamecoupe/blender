/*
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
 *
 * The Original Code is Copyright (C) 2010 Blender Foundation.
 * All rights reserved.
 */

/** \file
 * \ingroup wm
 *
 * Our own drag-and-drop, drag state and drop boxes.
 */

#include <string.h>

#include "DNA_screen_types.h"
#include "DNA_space_types.h"
#include "DNA_windowmanager_types.h"

#include "MEM_guardedalloc.h"

#include "BLT_translation.h"

#include "BLI_blenlib.h"

#include "BIF_glutil.h"

#include "BKE_context.h"
#include "BKE_global.h"
#include "BKE_idtype.h"
#include "BKE_lib_id.h"

#include "GPU_shader.h"
#include "GPU_state.h"
#include "GPU_viewport.h"

#include "IMB_imbuf_types.h"

#include "UI_interface.h"
#include "UI_interface_icons.h"

#include "RNA_access.h"

#include "WM_api.h"
#include "WM_types.h"
#include "wm_event_system.h"

/* ****************************************************** */

static ListBase dropboxes = {NULL, NULL};

/* drop box maps are stored global for now */
/* these are part of blender's UI/space specs, and not like keymaps */
/* when editors become configurable, they can add own dropbox definitions */

typedef struct wmDropBoxMap {
  struct wmDropBoxMap *next, *prev;

  ListBase dropboxes;
  short spaceid, regionid;
  char idname[KMAP_MAX_NAME];

} wmDropBoxMap;

/* spaceid/regionid is zero for window drop maps */
ListBase *WM_dropboxmap_find(const char *idname, int spaceid, int regionid)
{
  LISTBASE_FOREACH (wmDropBoxMap *, dm, &dropboxes) {
    if (dm->spaceid == spaceid && dm->regionid == regionid) {
      if (STREQLEN(idname, dm->idname, KMAP_MAX_NAME)) {
        return &dm->dropboxes;
      }
    }
  }

  wmDropBoxMap *dm = MEM_callocN(sizeof(struct wmDropBoxMap), "dropmap list");
  BLI_strncpy(dm->idname, idname, KMAP_MAX_NAME);
  dm->spaceid = spaceid;
  dm->regionid = regionid;
  BLI_addtail(&dropboxes, dm);

  return &dm->dropboxes;
}

wmDropBox *WM_dropbox_add(ListBase *lb,
                          const char *idname,
                          bool (*poll)(bContext *, wmDrag *, const wmEvent *, const char **),
                          void (*copy)(wmDrag *, wmDropBox *),
                          void (*cancel)(struct Main *, wmDrag *, wmDropBox *))
{
  wmDropBox *drop = MEM_callocN(sizeof(wmDropBox), "wmDropBox");
  drop->poll = poll;
  drop->copy = copy;
  drop->cancel = cancel;
  drop->ot = WM_operatortype_find(idname, 0);
  drop->opcontext = WM_OP_INVOKE_DEFAULT;

  if (drop->ot == NULL) {
    MEM_freeN(drop);
    printf("Error: dropbox with unknown operator: %s\n", idname);
    return NULL;
  }
  WM_operator_properties_alloc(&(drop->ptr), &(drop->properties), idname);

  BLI_addtail(lb, drop);

  return drop;
}

void wm_dropbox_free(void)
{

  LISTBASE_FOREACH (wmDropBoxMap *, dm, &dropboxes) {
    LISTBASE_FOREACH (wmDropBox *, drop, &dm->dropboxes) {
      if (drop->ptr) {
        WM_operator_properties_free(drop->ptr);
        MEM_freeN(drop->ptr);
      }
    }
    BLI_freelistN(&dm->dropboxes);
  }

  BLI_freelistN(&dropboxes);
}

/* *********************************** */

/* note that the pointer should be valid allocated and not on stack */
wmDrag *WM_event_start_drag(
    struct bContext *C, int icon, int type, void *poin, double value, unsigned int flags)
{
  wmWindowManager *wm = CTX_wm_manager(C);
  wmDrag *drag = MEM_callocN(sizeof(struct wmDrag), "new drag");

  /* Keep track of future multi-touch drag too, add a mouse-pointer id or so. */
  /* if multiple drags are added, they're drawn as list */

  BLI_addtail(&wm->drags, drag);
  drag->flags = flags;
  drag->icon = icon;
  drag->type = type;
  switch (type) {
    case WM_DRAG_PATH:
      BLI_strncpy(drag->path, poin, FILE_MAX);
      /* As the path is being copied, free it immediately as `drag` won't "own" the data. */
      if (flags & WM_DRAG_FREE_DATA) {
        MEM_freeN(poin);
      }
      break;
    case WM_DRAG_ID:
      if (poin) {
        WM_drag_add_local_ID(drag, poin, NULL);
      }
      break;
    case WM_DRAG_ASSET:
      /* Move ownership of poin to wmDrag. */
      drag->poin = poin;
      drag->flags |= WM_DRAG_FREE_DATA;
      break;
    default:
      drag->poin = poin;
      break;
  }
  drag->value = value;

  return drag;
}

void WM_event_drag_image(wmDrag *drag, ImBuf *imb, float scale, int sx, int sy)
{
  drag->imb = imb;
  drag->scale = scale;
  drag->sx = sx;
  drag->sy = sy;
}

void WM_drag_data_free(int dragtype, void *poin)
{
  /* Don't require all the callers to have a NULL-check, just allow passing NULL. */
  if (!poin) {
    return;
  }

  /* Not too nice, could become a callback. */
  if (dragtype == WM_DRAG_ASSET) {
    wmDragAsset *asset_drag = poin;
    MEM_freeN((void *)asset_drag->path);
  }
  MEM_freeN(poin);
}

void WM_drag_free(wmDrag *drag)
{
  if (drag->flags & WM_DRAG_FREE_DATA) {
    WM_drag_data_free(drag->type, drag->poin);
  }
  BLI_freelistN(&drag->ids);
  MEM_freeN(drag);
}

void WM_drag_free_list(struct ListBase *lb)
{
  wmDrag *drag;
  while ((drag = BLI_pophead(lb))) {
    WM_drag_free(drag);
  }
}

static const char *dropbox_active(bContext *C,
                                  ListBase *handlers,
                                  wmDrag *drag,
                                  const wmEvent *event)
{
  LISTBASE_FOREACH (wmEventHandler *, handler_base, handlers) {
    if (handler_base->type == WM_HANDLER_TYPE_DROPBOX) {
      wmEventHandler_Dropbox *handler = (wmEventHandler_Dropbox *)handler_base;
      if (handler->dropboxes) {
        LISTBASE_FOREACH (wmDropBox *, drop, handler->dropboxes) {
          const char *tooltip = NULL;
          if (drop->poll(C, drag, event, &tooltip) &&
              WM_operator_poll_context(C, drop->ot, drop->opcontext)) {
            /* XXX Doing translation here might not be ideal, but later we have no more
             *     access to ot (and hence op context)... */
            return (tooltip) ? tooltip : WM_operatortype_name(drop->ot, drop->ptr);
          }
        }
      }
    }
  }
  return NULL;
}

/* return active operator name when mouse is in box */
static const char *wm_dropbox_active(bContext *C, wmDrag *drag, const wmEvent *event)
{
  wmWindow *win = CTX_wm_window(C);
  ScrArea *area = CTX_wm_area(C);
  ARegion *region = CTX_wm_region(C);
  const char *name;

  name = dropbox_active(C, &win->handlers, drag, event);
  if (name) {
    return name;
  }

  name = dropbox_active(C, &area->handlers, drag, event);
  if (name) {
    return name;
  }

  name = dropbox_active(C, &region->handlers, drag, event);
  if (name) {
    return name;
  }

  return NULL;
}

static void wm_drop_operator_options(bContext *C, wmDrag *drag, const wmEvent *event)
{
  wmWindow *win = CTX_wm_window(C);
  const int winsize_x = WM_window_pixels_x(win);
  const int winsize_y = WM_window_pixels_y(win);

  /* for multiwin drags, we only do this if mouse inside */
  if (event->x < 0 || event->y < 0 || event->x > winsize_x || event->y > winsize_y) {
    return;
  }

  drag->opname[0] = 0;

  /* check buttons (XXX todo rna and value) */
  if (UI_but_active_drop_name(C)) {
    BLI_strncpy(drag->opname, IFACE_("Paste name"), sizeof(drag->opname));
  }
  else {
    const char *opname = wm_dropbox_active(C, drag, event);

    if (opname) {
      BLI_strncpy(drag->opname, opname, sizeof(drag->opname));
      // WM_cursor_modal_set(win, WM_CURSOR_COPY);
    }
    // else
    //  WM_cursor_modal_restore(win);
    /* unsure about cursor type, feels to be too much */
  }
}

/* called in inner handler loop, region context */
void wm_drags_check_ops(bContext *C, const wmEvent *event)
{
  wmWindowManager *wm = CTX_wm_manager(C);

  LISTBASE_FOREACH (wmDrag *, drag, &wm->drags) {
    wm_drop_operator_options(C, drag, event);
  }
}

/* ************** IDs ***************** */

void WM_drag_add_local_ID(wmDrag *drag, ID *id, ID *from_parent)
{
  /* Don't drag the same ID twice. */
  LISTBASE_FOREACH (wmDragID *, drag_id, &drag->ids) {
    if (drag_id->id == id) {
      if (drag_id->from_parent == NULL) {
        drag_id->from_parent = from_parent;
      }
      return;
    }
    if (GS(drag_id->id->name) != GS(id->name)) {
      BLI_assert(!"All dragged IDs must have the same type");
      return;
    }
  }

  /* Add to list. */
  wmDragID *drag_id = MEM_callocN(sizeof(wmDragID), __func__);
  drag_id->id = id;
  drag_id->from_parent = from_parent;
  BLI_addtail(&drag->ids, drag_id);
}

ID *WM_drag_get_local_ID(const wmDrag *drag, short idcode)
{
  if (drag->type != WM_DRAG_ID) {
    return NULL;
  }

  wmDragID *drag_id = drag->ids.first;
  if (!drag_id) {
    return NULL;
  }

  ID *id = drag_id->id;
  return (idcode == 0 || GS(id->name) == idcode) ? id : NULL;
}

ID *WM_drag_get_local_ID_from_event(const wmEvent *event, short idcode)
{
  if (event->custom != EVT_DATA_DRAGDROP) {
    return NULL;
  }

  ListBase *lb = event->customdata;
  return WM_drag_get_local_ID(lb->first, idcode);
}

/**
 * Check if the drag data is either a local ID or an external ID asset of type \a idcode.
 */
bool WM_drag_is_ID_type(const wmDrag *drag, int idcode)
{
  return WM_drag_get_local_ID(drag, idcode) || WM_drag_get_asset_data(drag, idcode);
}

wmDragAsset *WM_drag_get_asset_data(const wmDrag *drag, int idcode)
{
  if (drag->type != WM_DRAG_ASSET) {
    return NULL;
  }

  wmDragAsset *asset_drag = drag->poin;
  return (ELEM(idcode, 0, asset_drag->id_type)) ? asset_drag : NULL;
}

static ID *wm_drag_asset_id_import(wmDragAsset *asset_drag)
{
  switch ((eFileAssetImportType)asset_drag->import_type) {
    case FILE_ASSET_IMPORT_LINK:
      return WM_file_link_datablock(
          G_MAIN, NULL, NULL, NULL, asset_drag->path, asset_drag->id_type, asset_drag->name);
    case FILE_ASSET_IMPORT_APPEND:
      return WM_file_append_datablock(
          G_MAIN, NULL, NULL, NULL, asset_drag->path, asset_drag->id_type, asset_drag->name);
  }

  BLI_assert_unreachable();
  return NULL;
}

/**
 * When dragging a local ID, return that. Otherwise, if dragging an asset-handle, link or append
 * that depending on what was chosen by the drag-box (currently append only in fact).
 *
 * Use #WM_drag_free_imported_drag_ID() as cancel callback of the drop-box, so that the asset
 * import is rolled back if the drop operator fails.
 */
ID *WM_drag_get_local_ID_or_import_from_asset(const wmDrag *drag, int idcode)
{
  if (!ELEM(drag->type, WM_DRAG_ASSET, WM_DRAG_ID)) {
    return NULL;
  }

  if (drag->type == WM_DRAG_ID) {
    return WM_drag_get_local_ID(drag, idcode);
  }

  wmDragAsset *asset_drag = WM_drag_get_asset_data(drag, idcode);
  if (!asset_drag) {
    return NULL;
  }

  /* Link/append the asset. */
  return wm_drag_asset_id_import(asset_drag);
}

/**
 * \brief Free asset ID imported for cancelled drop.
 *
 * If the asset was imported (linked/appended) using #WM_drag_get_local_ID_or_import_from_asset()`
 * (typically via a #wmDropBox.copy() callback), we want the ID to be removed again if the drop
 * operator cancels.
 * This is for use as #wmDropBox.cancel() callback.
 */
void WM_drag_free_imported_drag_ID(struct Main *bmain, wmDrag *drag, wmDropBox *drop)
{
  if (drag->type != WM_DRAG_ASSET) {
    return;
  }

  wmDragAsset *asset_drag = WM_drag_get_asset_data(drag, 0);
  if (!asset_drag) {
    return;
  }

  /* Get name from property, not asset data - it may have changed after importing to ensure
   * uniqueness (name is assumed to be set from the imported ID name). */
  char name[MAX_ID_NAME - 2];
  RNA_string_get(drop->ptr, "name", name);
  if (!name[0]) {
    return;
  }

  ID *id = BKE_libblock_find_name(bmain, asset_drag->id_type, name);
  if (id) {
    BKE_id_delete(bmain, id);
  }
}

/* ************** draw ***************** */

static void wm_drop_operator_draw(const char *name, int x, int y)
{
  const uiFontStyle *fstyle = UI_FSTYLE_WIDGET;
  const float col_fg[4] = {1.0f, 1.0f, 1.0f, 1.0f};
  const float col_bg[4] = {0.0f, 0.0f, 0.0f, 0.2f};

  UI_fontstyle_draw_simple_backdrop(fstyle, x, y, name, col_fg, col_bg);
}

static const char *wm_drag_name(wmDrag *drag)
{
  switch (drag->type) {
    case WM_DRAG_ID: {
      ID *id = WM_drag_get_local_ID(drag, 0);
      bool single = (BLI_listbase_count_at_most(&drag->ids, 2) == 1);

      if (single) {
        return id->name + 2;
      }
      if (id) {
        return BKE_idtype_idcode_to_name_plural(GS(id->name));
      }
      break;
    }
    case WM_DRAG_ASSET: {
      const wmDragAsset *asset_drag = WM_drag_get_asset_data(drag, 0);
      return asset_drag->name;
    }
    case WM_DRAG_PATH:
    case WM_DRAG_NAME:
      return drag->path;
  }
  return "";
}

static void drag_rect_minmax(rcti *rect, int x1, int y1, int x2, int y2)
{
  if (rect->xmin > x1) {
    rect->xmin = x1;
  }
  if (rect->xmax < x2) {
    rect->xmax = x2;
  }
  if (rect->ymin > y1) {
    rect->ymin = y1;
  }
  if (rect->ymax < y2) {
    rect->ymax = y2;
  }
}

/* called in wm_draw.c */
/* if rect set, do not draw */
void wm_drags_draw(bContext *C, wmWindow *win, rcti *rect)
{
  const uiFontStyle *fstyle = UI_FSTYLE_WIDGET;
  wmWindowManager *wm = CTX_wm_manager(C);
  const int winsize_y = WM_window_pixels_y(win);

  int cursorx = win->eventstate->x;
  int cursory = win->eventstate->y;
  if (rect) {
    rect->xmin = rect->xmax = cursorx;
    rect->ymin = rect->ymax = cursory;
  }

  /* Should we support multi-line drag draws? Maybe not, more types mixed won't work well. */
  GPU_blend(GPU_BLEND_ALPHA);
  LISTBASE_FOREACH (wmDrag *, drag, &wm->drags) {
    const uchar text_col[] = {255, 255, 255, 255};
    int iconsize = UI_DPI_ICON_SIZE;
    int padding = 4 * UI_DPI_FAC;

    /* image or icon */
    int x, y;
    if (drag->imb) {
      x = cursorx - drag->sx / 2;
      y = cursory - drag->sy / 2;

      if (rect) {
        drag_rect_minmax(rect, x, y, x + drag->sx, y + drag->sy);
      }
      else {
        float col[4] = {1.0f, 1.0f, 1.0f, 0.65f}; /* this blends texture */
        IMMDrawPixelsTexState state = immDrawPixelsTexSetup(GPU_SHADER_2D_IMAGE_COLOR);
        immDrawPixelsTexScaled(&state,
                               x,
                               y,
                               drag->imb->x,
                               drag->imb->y,
                               GPU_RGBA8,
                               false,
                               drag->imb->rect,
                               drag->scale,
                               drag->scale,
                               1.0f,
                               1.0f,
                               col);
      }
    }
    else {
      x = cursorx - 2 * padding;
      y = cursory - 2 * UI_DPI_FAC;

      if (rect) {
        drag_rect_minmax(rect, x, y, x + iconsize, y + iconsize);
      }
      else {
        UI_icon_draw_ex(x, y, drag->icon, U.inv_dpi_fac, 0.8, 0.0f, text_col, false);
      }
    }

    /* item name */
    if (drag->imb) {
      x = cursorx - drag->sx / 2;
      y = cursory - drag->sy / 2 - iconsize;
    }
    else {
      x = cursorx + 10 * UI_DPI_FAC;
      y = cursory + 1 * UI_DPI_FAC;
    }

    if (rect) {
      int w = UI_fontstyle_string_width(fstyle, wm_drag_name(drag));
      drag_rect_minmax(rect, x, y, x + w, y + iconsize);
    }
    else {
      UI_fontstyle_draw_simple(fstyle, x, y, wm_drag_name(drag), text_col);
    }

    /* operator name with roundbox */
    if (drag->opname[0]) {
      if (drag->imb) {
        x = cursorx - drag->sx / 2;

        if (cursory + drag->sy / 2 + padding + iconsize < winsize_y) {
          y = cursory + drag->sy / 2 + padding;
        }
        else {
          y = cursory - drag->sy / 2 - padding - iconsize - padding - iconsize;
        }
      }
      else {
        x = cursorx - 2 * padding;

        if (cursory + iconsize + iconsize < winsize_y) {
          y = (cursory + iconsize) + padding;
        }
        else {
          y = (cursory - iconsize) - padding;
        }
      }

      if (rect) {
        int w = UI_fontstyle_string_width(fstyle, wm_drag_name(drag));
        drag_rect_minmax(rect, x, y, x + w, y + iconsize);
      }
      else {
        wm_drop_operator_draw(drag->opname, x, y);
      }
    }
  }
  GPU_blend(GPU_BLEND_NONE);
}
