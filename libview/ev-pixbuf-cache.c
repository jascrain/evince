#include <config.h>
#include "ev-pixbuf-cache.h"
#include "ev-job-scheduler.h"
#include "ev-mapping.h"
#include "ev-document-forms.h"
#include "ev-document-images.h"
#include "ev-document-annotations.h"
#include "ev-view-private.h"

typedef struct _CacheJobInfo
{
	EvJob *job;
	EvRenderContext *rc;
	gboolean page_ready;

	/* Region of the page that needs to be drawn */
	GdkRegion *region; 

	/* Data we get from rendering */
	cairo_surface_t *surface;

	/* Selection data. 
	 * Selection_points are the coordinates encapsulated in selection.
	 * target_points is the target selection size. */
	EvRectangle      selection_points;
	EvRectangle      target_points;
	EvSelectionStyle selection_style;
	gboolean         points_set;
	
	cairo_surface_t *selection;
	GdkRegion *selection_region;
} CacheJobInfo;

struct _EvPixbufCache
{
	GObject parent;

	/* We keep a link to our containing view just for style information. */
	GtkWidget *view;
	EvDocument *document;
	int start_page;
	int end_page;
	gboolean inverted_colors;

	/* preload_cache_size is the number of pages prior to the current
	 * visible area that we cache.  It's normally 1, but could be 2 in the
	 * case of twin pages.
	 */
	int preload_cache_size;
	CacheJobInfo *prev_job;
	CacheJobInfo *job_list;
	CacheJobInfo *next_job;
};

struct _EvPixbufCacheClass
{
	GObjectClass parent_class;

	void (* job_finished) (EvPixbufCache *pixbuf_cache);
};


enum
{
	JOB_FINISHED,
	N_SIGNALS,
};

static guint signals[N_SIGNALS] = {0, };

static void          ev_pixbuf_cache_init       (EvPixbufCache      *pixbuf_cache);
static void          ev_pixbuf_cache_class_init (EvPixbufCacheClass *pixbuf_cache);
static void          ev_pixbuf_cache_finalize   (GObject            *object);
static void          ev_pixbuf_cache_dispose    (GObject            *object);
static void          job_finished_cb            (EvJob              *job,
						 EvPixbufCache      *pixbuf_cache);
static CacheJobInfo *find_job_cache             (EvPixbufCache      *pixbuf_cache,
						 int                 page);
static gboolean      new_selection_surface_needed(EvPixbufCache      *pixbuf_cache,
						  CacheJobInfo       *job_info,
						  gint                page,
						  gfloat              scale);


/* These are used for iterating through the prev and next arrays */
#define FIRST_VISIBLE_PREV(pixbuf_cache) \
	(MAX (0, pixbuf_cache->preload_cache_size - pixbuf_cache->start_page))
#define VISIBLE_NEXT_LEN(pixbuf_cache) \
	(MIN(pixbuf_cache->preload_cache_size, ev_document_get_n_pages (pixbuf_cache->document) - (1 + pixbuf_cache->end_page)))
#define PAGE_CACHE_LEN(pixbuf_cache) \
	((pixbuf_cache->end_page - pixbuf_cache->start_page) + 1)

G_DEFINE_TYPE (EvPixbufCache, ev_pixbuf_cache, G_TYPE_OBJECT)

static void
ev_pixbuf_cache_init (EvPixbufCache *pixbuf_cache)
{
	pixbuf_cache->start_page = 0;
	pixbuf_cache->end_page = 0;
	pixbuf_cache->job_list = g_new0 (CacheJobInfo, PAGE_CACHE_LEN (pixbuf_cache));

	pixbuf_cache->preload_cache_size = 2;
	pixbuf_cache->prev_job = g_new0 (CacheJobInfo, pixbuf_cache->preload_cache_size);
	pixbuf_cache->next_job = g_new0 (CacheJobInfo, pixbuf_cache->preload_cache_size);
}

static void
ev_pixbuf_cache_class_init (EvPixbufCacheClass *class)
{
	GObjectClass *object_class;

	object_class = G_OBJECT_CLASS (class);

	object_class->finalize = ev_pixbuf_cache_finalize;
	object_class->dispose = ev_pixbuf_cache_dispose;

	signals[JOB_FINISHED] =
		g_signal_new ("job-finished",
			      G_OBJECT_CLASS_TYPE (object_class),
			      G_SIGNAL_RUN_LAST | G_SIGNAL_ACTION,
			      G_STRUCT_OFFSET (EvPixbufCacheClass, job_finished),
			      NULL, NULL,
			      g_cclosure_marshal_VOID__POINTER,
			      G_TYPE_NONE, 1,
			      G_TYPE_POINTER);
}

static void
ev_pixbuf_cache_finalize (GObject *object)
{
	EvPixbufCache *pixbuf_cache;

	pixbuf_cache = EV_PIXBUF_CACHE (object);

	g_free (pixbuf_cache->prev_job);
	g_free (pixbuf_cache->job_list);
	g_free (pixbuf_cache->next_job);

	G_OBJECT_CLASS (ev_pixbuf_cache_parent_class)->finalize (object);
}

static void
dispose_cache_job_info (CacheJobInfo *job_info,
			gpointer      data)
{
	if (job_info == NULL)
		return;

	if (job_info->job) {
		g_signal_handlers_disconnect_by_func (job_info->job,
						      G_CALLBACK (job_finished_cb),
						      data);
		ev_job_cancel (job_info->job);
		g_object_unref (job_info->job);
		job_info->job = NULL;
	}
	if (job_info->surface) {
		cairo_surface_destroy (job_info->surface);
		job_info->surface = NULL;
	}
	if (job_info->region) {
		gdk_region_destroy (job_info->region);
		job_info->region = NULL;
	}
	if (job_info->selection) {
		cairo_surface_destroy (job_info->selection);
		job_info->selection = NULL;
	}
	if (job_info->selection_region) {
		gdk_region_destroy (job_info->selection_region);
		job_info->selection_region = NULL;
	}
	if (job_info->rc) {
		g_object_unref (G_OBJECT (job_info->rc));
		job_info->rc = NULL;
	}

	job_info->points_set = FALSE;
}

static void
ev_pixbuf_cache_dispose (GObject *object)
{
	EvPixbufCache *pixbuf_cache;
	int i;

	pixbuf_cache = EV_PIXBUF_CACHE (object);

	for (i = 0; i < pixbuf_cache->preload_cache_size; i++) {
		dispose_cache_job_info (pixbuf_cache->prev_job + i, pixbuf_cache);
		dispose_cache_job_info (pixbuf_cache->next_job + i, pixbuf_cache);
	}

	for (i = 0; i < PAGE_CACHE_LEN (pixbuf_cache); i++) {
		dispose_cache_job_info (pixbuf_cache->job_list + i, pixbuf_cache);
	}

	G_OBJECT_CLASS (ev_pixbuf_cache_parent_class)->dispose (object);
}


EvPixbufCache *
ev_pixbuf_cache_new (GtkWidget  *view,
		     EvDocument *document)
{
	EvPixbufCache *pixbuf_cache;

	pixbuf_cache = (EvPixbufCache *) g_object_new (EV_TYPE_PIXBUF_CACHE, NULL);
	/* This is a backlink, so we don't ref this */ 
	pixbuf_cache->view = view;
	pixbuf_cache->document = document;

	return pixbuf_cache;
}

static void
copy_job_to_job_info (EvJobRender   *job_render,
		      CacheJobInfo  *job_info,
		      EvPixbufCache *pixbuf_cache)
{
	if (job_info->rc == NULL) {
		job_info->rc = ev_render_context_new (job_render->ev_page,
						      job_render->rotation,
						      job_render->scale);
	} else {
		ev_render_context_set_page (job_info->rc, job_render->ev_page);
		ev_render_context_set_rotation (job_info->rc, job_render->rotation);
		ev_render_context_set_scale (job_info->rc, job_render->scale);
	}

	if (job_info->surface) {
		cairo_surface_destroy (job_info->surface);
	}
	job_info->surface = cairo_surface_reference (job_render->surface);
	if (pixbuf_cache->inverted_colors) {
		ev_document_misc_invert_surface (job_info->surface);
	}

	job_info->points_set = FALSE;
	if (job_render->include_selection) {
		if (job_info->selection) {
			cairo_surface_destroy (job_info->selection);
			job_info->selection = NULL;
		}
		if (job_info->selection_region) {
			gdk_region_destroy (job_info->selection_region);
			job_info->selection_region = NULL;
		}

		job_info->selection_points = job_render->selection_points;
		job_info->selection_region = gdk_region_copy (job_render->selection_region);
		job_info->selection = cairo_surface_reference (job_render->selection);
		g_assert (job_info->selection_points.x1 >= 0);
		job_info->points_set = TRUE;
	}

	if (job_info->job) {
		g_signal_handlers_disconnect_by_func (job_info->job,
						      G_CALLBACK (job_finished_cb),
						      pixbuf_cache);
		ev_job_cancel (job_info->job);
		g_object_unref (job_info->job);
		job_info->job = NULL;
	}

	job_info->page_ready = TRUE;
}

static void
job_finished_cb (EvJob         *job,
		 EvPixbufCache *pixbuf_cache)
{
	CacheJobInfo *job_info;
	EvJobRender *job_render = EV_JOB_RENDER (job);

	/* If the job is outside of our interest, we silently discard it */
	if ((job_render->page < (pixbuf_cache->start_page - pixbuf_cache->preload_cache_size)) ||
	    (job_render->page > (pixbuf_cache->end_page + pixbuf_cache->preload_cache_size))) {
		g_object_unref (job);
		return;
	}

	job_info = find_job_cache (pixbuf_cache, job_render->page);

	copy_job_to_job_info (job_render, job_info, pixbuf_cache);
	g_signal_emit (pixbuf_cache, signals[JOB_FINISHED], 0, job_info->region);
}

/* This checks a job to see if the job would generate the right sized pixbuf
 * given a scale.  If it won't, it removes the job and clears it to NULL.
 */
static void
check_job_size_and_unref (EvPixbufCache *pixbuf_cache,
			  CacheJobInfo  *job_info,
			  gfloat         scale)
{
	gint width, height;

	g_assert (job_info);

	if (job_info->job == NULL)
		return;

	_get_page_size_for_scale_and_rotation (job_info->job->document,
					       EV_JOB_RENDER (job_info->job)->page,
					       scale,
					       EV_JOB_RENDER (job_info->job)->rotation,
					       &width, &height);
	if (width == EV_JOB_RENDER (job_info->job)->target_width &&
	    height == EV_JOB_RENDER (job_info->job)->target_height)
		return;

	g_signal_handlers_disconnect_by_func (job_info->job,
					      G_CALLBACK (job_finished_cb),
					      pixbuf_cache);
	ev_job_cancel (job_info->job);
	g_object_unref (job_info->job);
	job_info->job = NULL;
}

/* Do all function that copies a job from an older cache to it's position in the
 * new cache.  It clears the old job if it doesn't have a place.
 */
static void
move_one_job (CacheJobInfo  *job_info,
	      EvPixbufCache *pixbuf_cache,
	      int            page,
	      CacheJobInfo  *new_job_list,
	      CacheJobInfo  *new_prev_job,
	      CacheJobInfo  *new_next_job,
	      int            start_page,
	      int            end_page,
	      gint           priority)
{
	CacheJobInfo *target_page = NULL;
	int page_offset;
	gint new_priority;

	if (page < (start_page - pixbuf_cache->preload_cache_size) ||
	    page > (end_page + pixbuf_cache->preload_cache_size)) {
		dispose_cache_job_info (job_info, pixbuf_cache);
		return;
	}

	/* find the target page to copy it over to. */
	if (page < start_page) {
		page_offset = (page - (start_page - pixbuf_cache->preload_cache_size));

		g_assert (page_offset >= 0 &&
			  page_offset < pixbuf_cache->preload_cache_size);
		target_page = new_prev_job + page_offset;
		new_priority = EV_JOB_PRIORITY_LOW;
	} else if (page > end_page) {
		page_offset = (page - (end_page + 1));

		g_assert (page_offset >= 0 &&
			  page_offset < pixbuf_cache->preload_cache_size);
		target_page = new_next_job + page_offset;
		new_priority = EV_JOB_PRIORITY_LOW;
	} else {
		page_offset = page - start_page;
		g_assert (page_offset >= 0 &&
			  page_offset <= ((end_page - start_page) + 1));
		new_priority = EV_JOB_PRIORITY_URGENT;
		target_page = new_job_list + page_offset;
	}

	*target_page = *job_info;
	job_info->job = NULL;
	job_info->region = NULL;
	job_info->surface = NULL;

	if (new_priority != priority && target_page->job) {
		ev_job_scheduler_update_job (target_page->job, new_priority);
	}
}

static void
ev_pixbuf_cache_update_range (EvPixbufCache *pixbuf_cache,
			      gint           start_page,
			      gint           end_page)
{
	CacheJobInfo *new_job_list;
	CacheJobInfo *new_prev_job;
	CacheJobInfo *new_next_job;
	int i, page;

	if (pixbuf_cache->start_page == start_page &&
	    pixbuf_cache->end_page == end_page)
		return;

	new_job_list = g_new0 (CacheJobInfo, (end_page - start_page) + 1);
	new_prev_job = g_new0 (CacheJobInfo, pixbuf_cache->preload_cache_size);
	new_next_job = g_new0 (CacheJobInfo, pixbuf_cache->preload_cache_size);

	/* We go through each job in the old cache and either clear it or move
	 * it to a new location. */

	/* Start with the prev cache. */
	page = pixbuf_cache->start_page - pixbuf_cache->preload_cache_size;
	for (i = 0; i < pixbuf_cache->preload_cache_size; i++) {
		if (page < 0) {
			dispose_cache_job_info (pixbuf_cache->prev_job + i, pixbuf_cache);
		} else {
			move_one_job (pixbuf_cache->prev_job + i,
				      pixbuf_cache, page,
				      new_job_list, new_prev_job, new_next_job,
				      start_page, end_page, EV_JOB_PRIORITY_LOW);
		}
		page ++;
	}

	page = pixbuf_cache->start_page;
	for (i = 0; i < PAGE_CACHE_LEN (pixbuf_cache); i++) {
		move_one_job (pixbuf_cache->job_list + i,
			      pixbuf_cache, page,
			      new_job_list, new_prev_job, new_next_job,
			      start_page, end_page, EV_JOB_PRIORITY_URGENT);
		page ++;
	}

	for (i = 0; i < pixbuf_cache->preload_cache_size; i++) {
		if (page >= ev_document_get_n_pages (pixbuf_cache->document)) {
			dispose_cache_job_info (pixbuf_cache->next_job + i, pixbuf_cache);
		} else {
			move_one_job (pixbuf_cache->next_job + i,
				      pixbuf_cache, page,
				      new_job_list, new_prev_job, new_next_job,
				      start_page, end_page, EV_JOB_PRIORITY_LOW);
		}
		page ++;
	}

	g_free (pixbuf_cache->job_list);
	g_free (pixbuf_cache->prev_job);
	g_free (pixbuf_cache->next_job);

	pixbuf_cache->job_list = new_job_list;
	pixbuf_cache->prev_job = new_prev_job;
	pixbuf_cache->next_job = new_next_job;

	pixbuf_cache->start_page = start_page;
	pixbuf_cache->end_page = end_page;
}

static CacheJobInfo *
find_job_cache (EvPixbufCache *pixbuf_cache,
		int            page)
{
	int page_offset;

	if (page < (pixbuf_cache->start_page - pixbuf_cache->preload_cache_size) ||
	    page > (pixbuf_cache->end_page + pixbuf_cache->preload_cache_size))
		return NULL;

	if (page < pixbuf_cache->start_page) {
		page_offset = (page - (pixbuf_cache->start_page - pixbuf_cache->preload_cache_size));

		g_assert (page_offset >= 0 &&
			  page_offset < pixbuf_cache->preload_cache_size);
		return pixbuf_cache->prev_job + page_offset;
	}

	if (page > pixbuf_cache->end_page) {
		page_offset = (page - (pixbuf_cache->end_page + 1));

		g_assert (page_offset >= 0 &&
			  page_offset < pixbuf_cache->preload_cache_size);
		return pixbuf_cache->next_job + page_offset;
	}

	page_offset = page - pixbuf_cache->start_page;
	g_assert (page_offset >= 0 &&
		  page_offset <= PAGE_CACHE_LEN(pixbuf_cache));
	return pixbuf_cache->job_list + page_offset;
}

static void
ev_pixbuf_cache_clear_job_sizes (EvPixbufCache *pixbuf_cache,
				 gfloat         scale)
{
	int i;

	for (i = 0; i < PAGE_CACHE_LEN (pixbuf_cache); i++) {
		check_job_size_and_unref (pixbuf_cache, pixbuf_cache->job_list + i, scale);
	}

	for (i = 0; i < pixbuf_cache->preload_cache_size; i++) {
		check_job_size_and_unref (pixbuf_cache, pixbuf_cache->prev_job + i, scale);
		check_job_size_and_unref (pixbuf_cache, pixbuf_cache->next_job + i, scale);
	}
}

static void
get_selection_colors (GtkWidget *widget, GdkColor **text, GdkColor **base)
{
    if (GTK_WIDGET_HAS_FOCUS (widget)) {
	*text = &widget->style->text [GTK_STATE_SELECTED];
	*base = &widget->style->base [GTK_STATE_SELECTED];
    } else {
	*text = &widget->style->text [GTK_STATE_ACTIVE];
	*base = &widget->style->base [GTK_STATE_ACTIVE];
    }
}

static void
add_job (EvPixbufCache *pixbuf_cache,
	 CacheJobInfo  *job_info,
	 GdkRegion     *region,
	 gint           width,
	 gint           height,
	 gint           page,
	 gint           rotation,
	 gfloat         scale,
	 EvJobPriority  priority)
{
	job_info->page_ready = FALSE;

	if (job_info->region)
		gdk_region_destroy (job_info->region);
	job_info->region = region ? gdk_region_copy (region) : NULL;

	job_info->job = ev_job_render_new (pixbuf_cache->document,
					   page, rotation, scale,
					   width, height);

	if (new_selection_surface_needed (pixbuf_cache, job_info, page, scale)) {
		GdkColor *text, *base;

		gtk_widget_ensure_style (pixbuf_cache->view);
		get_selection_colors (pixbuf_cache->view, &text, &base);
		ev_job_render_set_selection_info (EV_JOB_RENDER (job_info->job), 
						  &(job_info->target_points),
						  job_info->selection_style,
						  text, base);
	}

	g_signal_connect (job_info->job, "finished",
			  G_CALLBACK (job_finished_cb),
			  pixbuf_cache);
	ev_job_scheduler_push_job (job_info->job, priority);
}

static void
add_job_if_needed (EvPixbufCache *pixbuf_cache,
		   CacheJobInfo  *job_info,
		   gint           page,
		   gint           rotation,
		   gfloat         scale,
		   EvJobPriority  priority)
{
	gint width, height;

	if (job_info->job)
		return;

	_get_page_size_for_scale_and_rotation (pixbuf_cache->document,
					       page, scale, rotation,
					       &width, &height);

	if (job_info->surface &&
	    cairo_image_surface_get_width (job_info->surface) == width &&
	    cairo_image_surface_get_height (job_info->surface) == height)
		return;

	add_job (pixbuf_cache, job_info, NULL,
		 width, height, page, rotation, scale,
		 priority);
}

static void
ev_pixbuf_cache_add_jobs_if_needed (EvPixbufCache *pixbuf_cache,
				    gint           rotation,
				    gfloat         scale)
{
	CacheJobInfo *job_info;
	int page;
	int i;

	for (i = 0; i < PAGE_CACHE_LEN (pixbuf_cache); i++) {
		job_info = (pixbuf_cache->job_list + i);
		page = pixbuf_cache->start_page + i;

		add_job_if_needed (pixbuf_cache, job_info,
				   page, rotation, scale,
				   EV_JOB_PRIORITY_URGENT);
	}

	for (i = FIRST_VISIBLE_PREV(pixbuf_cache); i < pixbuf_cache->preload_cache_size; i++) {
		job_info = (pixbuf_cache->prev_job + i);
		page = pixbuf_cache->start_page - pixbuf_cache->preload_cache_size + i;

		add_job_if_needed (pixbuf_cache, job_info,
				   page, rotation, scale,
				   EV_JOB_PRIORITY_LOW);
	}

	for (i = 0; i < VISIBLE_NEXT_LEN(pixbuf_cache); i++) {
		job_info = (pixbuf_cache->next_job + i);
		page = pixbuf_cache->end_page + 1 + i;

		add_job_if_needed (pixbuf_cache, job_info,
				   page, rotation, scale,
				   EV_JOB_PRIORITY_LOW);
	}

}

void
ev_pixbuf_cache_set_page_range (EvPixbufCache  *pixbuf_cache,
				gint            start_page,
				gint            end_page,
				gint            rotation,
				gfloat          scale,
				GList          *selection_list)
{
	g_return_if_fail (EV_IS_PIXBUF_CACHE (pixbuf_cache));

	g_return_if_fail (start_page >= 0 && start_page < ev_document_get_n_pages (pixbuf_cache->document));
	g_return_if_fail (end_page >= 0 && end_page < ev_document_get_n_pages (pixbuf_cache->document));
	g_return_if_fail (end_page >= start_page);

	/* First, resize the page_range as needed.  We cull old pages
	 * mercilessly. */
	ev_pixbuf_cache_update_range (pixbuf_cache, start_page, end_page);

	/* Then, we update the current jobs to see if any of them are the wrong
	 * size, we remove them if we need to. */
	ev_pixbuf_cache_clear_job_sizes (pixbuf_cache, scale);

	/* Next, we update the target selection for our pages */
	ev_pixbuf_cache_set_selection_list (pixbuf_cache, selection_list);

	/* Finally, we add the new jobs for all the sizes that don't have a
	 * pixbuf */
	ev_pixbuf_cache_add_jobs_if_needed (pixbuf_cache, rotation, scale);
}

void
ev_pixbuf_cache_set_inverted_colors (EvPixbufCache *pixbuf_cache,
				     gboolean       inverted_colors)
{
	gint i;

	if (pixbuf_cache->inverted_colors == inverted_colors)
		return;

	pixbuf_cache->inverted_colors = inverted_colors;

	for (i = 0; i < pixbuf_cache->preload_cache_size; i++) {
		CacheJobInfo *job_info;

		job_info = pixbuf_cache->prev_job + i;
		if (job_info->surface)
			ev_document_misc_invert_surface (job_info->surface);

		job_info = pixbuf_cache->next_job + i;
		if (job_info->surface)
			ev_document_misc_invert_surface (job_info->surface);
	}

	for (i = 0; i < PAGE_CACHE_LEN (pixbuf_cache); i++) {
		CacheJobInfo *job_info;

		job_info = pixbuf_cache->job_list + i;
		if (job_info->surface)
			ev_document_misc_invert_surface (job_info->surface);
	}
}

cairo_surface_t *
ev_pixbuf_cache_get_surface (EvPixbufCache *pixbuf_cache,
			     gint           page)
{
	CacheJobInfo *job_info;

	job_info = find_job_cache (pixbuf_cache, page);
	if (job_info == NULL)
		return NULL;

	if (job_info->page_ready)
		return job_info->surface;

	/* We don't need to wait for the idle to handle the callback */
	if (job_info->job &&
	    EV_JOB_RENDER (job_info->job)->page_ready) {
		copy_job_to_job_info (EV_JOB_RENDER (job_info->job), job_info, pixbuf_cache);
		g_signal_emit (pixbuf_cache, signals[JOB_FINISHED], 0, job_info->region);
	}

	return job_info->surface;
}

static gboolean
new_selection_surface_needed (EvPixbufCache *pixbuf_cache,
			      CacheJobInfo  *job_info,
			      gint           page,
			      gfloat         scale)
{
	if (job_info->selection && job_info->rc) {
		gint width, height;
		gint selection_width, selection_height;

		_get_page_size_for_scale_and_rotation (pixbuf_cache->document,
						       page, scale, job_info->rc->rotation,
						       &width, &height);

		selection_width = cairo_image_surface_get_width (job_info->selection);
		selection_height = cairo_image_surface_get_height (job_info->selection);
		
		if (width != selection_width || height != selection_height)
			return TRUE;
	} else {
		if (job_info->points_set)
			return TRUE;
	}
	
	return FALSE;
}

static void
clear_selection_if_needed (EvPixbufCache *pixbuf_cache,
			   CacheJobInfo  *job_info,
			   gint           page,
			   gfloat         scale)
{
	if (new_selection_surface_needed (pixbuf_cache, job_info, page, scale)) {
		if (job_info->selection)
			cairo_surface_destroy (job_info->selection);
		job_info->selection = NULL;
		job_info->selection_points.x1 = -1;
	}
}

/* Clears the cache of jobs and pixbufs.
 */
void
ev_pixbuf_cache_clear (EvPixbufCache *pixbuf_cache)
{
	int i;

	for (i = 0; i < pixbuf_cache->preload_cache_size; i++) {
		dispose_cache_job_info (pixbuf_cache->prev_job + i, pixbuf_cache);
		dispose_cache_job_info (pixbuf_cache->next_job + i, pixbuf_cache);
	}

	for (i = 0; i < PAGE_CACHE_LEN (pixbuf_cache); i++) {
		dispose_cache_job_info (pixbuf_cache->job_list + i, pixbuf_cache);
	}
}


void
ev_pixbuf_cache_style_changed (EvPixbufCache *pixbuf_cache)
{
	gint i;

	/* FIXME: doesn't update running jobs. */
	for (i = 0; i < pixbuf_cache->preload_cache_size; i++) {
		CacheJobInfo *job_info;

		job_info = pixbuf_cache->prev_job + i;
		if (job_info->selection) {
			cairo_surface_destroy (job_info->selection);
			job_info->selection = NULL;
		}

		job_info = pixbuf_cache->next_job + i;
		if (job_info->selection) {
			cairo_surface_destroy (job_info->selection);
			job_info->selection = NULL;
		}
	}

	for (i = 0; i < PAGE_CACHE_LEN (pixbuf_cache); i++) {
		CacheJobInfo *job_info;

		job_info = pixbuf_cache->job_list + i;
		if (job_info->selection) {
			cairo_surface_destroy (job_info->selection);
			job_info->selection = NULL;
		}
	}
}

cairo_surface_t *
ev_pixbuf_cache_get_selection_surface (EvPixbufCache  *pixbuf_cache,
				       gint            page,
				       gfloat          scale,
				       GdkRegion     **region)
{
	CacheJobInfo *job_info;

	/* the document does not implement the selection interface */
	if (!EV_IS_SELECTION (pixbuf_cache->document))
		return NULL;

	job_info = find_job_cache (pixbuf_cache, page);
	if (job_info == NULL)
		return NULL;

	/* No selection on this page */
	if (!job_info->points_set)
		return NULL;

	/* Create new render context if needed (selection + fast scrolling) */
	if (job_info->rc == NULL) {
		EvPage  *ev_page;
		ev_page = ev_document_get_page (pixbuf_cache->document, page);
		job_info->rc = ev_render_context_new (ev_page, 0, scale);
		g_object_unref (ev_page);
	}

	/* Update the rc */
	ev_render_context_set_scale (job_info->rc, scale);

	/* If we have a running job, we just return what we have under the
	 * assumption that it'll be updated later and we can scale it as need
	 * be */
	if (job_info->job && EV_JOB_RENDER (job_info->job)->include_selection)
		return job_info->selection;

	/* Now, lets see if we need to resize the image.  If we do, we clear the
	 * old one. */
	clear_selection_if_needed (pixbuf_cache, job_info, page, scale);

	/* Finally, we see if the two scales are the same, and get a new pixbuf
	 * if needed.  We do this synchronously for now.  At some point, we
	 * _should_ be able to get rid of the doc_mutex, so the synchronicity
	 * doesn't kill us.  Rendering a few glyphs should really be fast.
	 */
	if (ev_rect_cmp (&(job_info->target_points), &(job_info->selection_points))) {
		EvRectangle *old_points;
		GdkColor *text, *base;

		/* we need to get a new selection pixbuf */
		ev_document_doc_mutex_lock ();
		if (job_info->selection_points.x1 < 0) {
			g_assert (job_info->selection == NULL);
			old_points = NULL;
		} else {
			g_assert (job_info->selection != NULL);
			old_points = &(job_info->selection_points);
		}

		if (job_info->selection_region)
			gdk_region_destroy (job_info->selection_region);
		job_info->selection_region =
			ev_selection_get_selection_region (EV_SELECTION (pixbuf_cache->document),
							   job_info->rc,
							   job_info->selection_style,
							   &(job_info->target_points));

		gtk_widget_ensure_style (pixbuf_cache->view);

		get_selection_colors (pixbuf_cache->view, &text, &base);

		ev_selection_render_selection (EV_SELECTION (pixbuf_cache->document),
					       job_info->rc, &(job_info->selection),
					       &(job_info->target_points),
					       old_points,
					       job_info->selection_style,
					       text, base);
		job_info->selection_points = job_info->target_points;
		ev_document_doc_mutex_unlock ();
	}
	if (region)
		*region = job_info->selection_region;
	return job_info->selection;
}

static void
update_job_selection (CacheJobInfo    *job_info,
		      EvViewSelection *selection)
{
	job_info->points_set = TRUE;		
	job_info->target_points = selection->rect;
	job_info->selection_style = selection->style;
}

static void
clear_job_selection (CacheJobInfo *job_info)
{
	job_info->points_set = FALSE;
	job_info->selection_points.x1 = -1;

	if (job_info->selection) {
		cairo_surface_destroy (job_info->selection);
		job_info->selection = NULL;
	}
}

/* This function will reset the selection on pages that no longer have them, and
 * will update the target_selection on those that need it.  It will _not_ free
 * the previous selection_list -- that's up to caller to do.
 */
void
ev_pixbuf_cache_set_selection_list (EvPixbufCache *pixbuf_cache,
				    GList         *selection_list)
{
	EvViewSelection *selection;
	GList *list = selection_list;
	int page;
	int i;

	g_return_if_fail (EV_IS_PIXBUF_CACHE (pixbuf_cache));

	if (!EV_IS_SELECTION (pixbuf_cache->document))
		return;

	/* We check each area to see what needs updating, and what needs freeing; */
	page = pixbuf_cache->start_page - pixbuf_cache->preload_cache_size;
	for (i = 0; i < pixbuf_cache->preload_cache_size; i++) {
		if (page < 0) {
			page ++;
			continue;
		}

		selection = NULL;
		while (list) {
			if (((EvViewSelection *)list->data)->page == page) {
				selection = list->data;
				break;
			} else if (((EvViewSelection *)list->data)->page > page) 
				break;
			list = list->next;
		}

		if (selection)
			update_job_selection (pixbuf_cache->prev_job + i, selection);
		else
			clear_job_selection (pixbuf_cache->prev_job + i);
		page ++;
	}

	page = pixbuf_cache->start_page;
	for (i = 0; i < PAGE_CACHE_LEN (pixbuf_cache); i++) {
		selection = NULL;
		while (list) {
			if (((EvViewSelection *)list->data)->page == page) {
				selection = list->data;
				break;
			} else if (((EvViewSelection *)list->data)->page > page) 
				break;
			list = list->next;
		}

		if (selection)
			update_job_selection (pixbuf_cache->job_list + i, selection);
		else
			clear_job_selection (pixbuf_cache->job_list + i);
		page ++;
	}

	for (i = 0; i < pixbuf_cache->preload_cache_size; i++) {
		if (page >= ev_document_get_n_pages (pixbuf_cache->document))
			break;

		selection = NULL;
		while (list) {
			if (((EvViewSelection *)list->data)->page == page) {
				selection = list->data;
				break;
			} else if (((EvViewSelection *)list->data)->page > page) 
				break;
			list = list->next;
		}

		if (selection)
			update_job_selection (pixbuf_cache->next_job + i, selection);
		else
			clear_job_selection (pixbuf_cache->next_job + i);
		page ++;
	}
}


/* Returns what the pixbuf cache thinks is */

GList *
ev_pixbuf_cache_get_selection_list (EvPixbufCache *pixbuf_cache)
{
	EvViewSelection *selection;
	GList *retval = NULL;
	int page;
	int i;

	g_return_val_if_fail (EV_IS_PIXBUF_CACHE (pixbuf_cache), NULL);

	/* We check each area to see what needs updating, and what needs freeing; */
	page = pixbuf_cache->start_page - pixbuf_cache->preload_cache_size;
	for (i = 0; i < pixbuf_cache->preload_cache_size; i++) {
		if (page < 0) {
			page ++;
			continue;
		}

		if (pixbuf_cache->prev_job[i].selection_points.x1 != -1) {
			selection = g_new0 (EvViewSelection, 1);
			selection->page = page;
			selection->rect = pixbuf_cache->prev_job[i].selection_points;
			if (pixbuf_cache->prev_job[i].selection_region)
				selection->covered_region = gdk_region_copy (pixbuf_cache->prev_job[i].selection_region);
			retval = g_list_append (retval, selection);
		}
		
		page ++;
	}

	page = pixbuf_cache->start_page;
	for (i = 0; i < PAGE_CACHE_LEN (pixbuf_cache); i++) {
		if (pixbuf_cache->job_list[i].selection_points.x1 != -1) {
			selection = g_new0 (EvViewSelection, 1);
			selection->page = page;
			selection->rect = pixbuf_cache->job_list[i].selection_points;
			if (pixbuf_cache->job_list[i].selection_region)
				selection->covered_region = gdk_region_copy (pixbuf_cache->job_list[i].selection_region);
			retval = g_list_append (retval, selection);
		}
		
		page ++;
	}

	for (i = 0; i < pixbuf_cache->preload_cache_size; i++) {
		if (page >= ev_document_get_n_pages (pixbuf_cache->document))
			break;

		if (pixbuf_cache->next_job[i].selection_points.x1 != -1) {
			selection = g_new0 (EvViewSelection, 1);
			selection->page = page;
			selection->rect = pixbuf_cache->next_job[i].selection_points;
			if (pixbuf_cache->next_job[i].selection_region)
				selection->covered_region = gdk_region_copy (pixbuf_cache->next_job[i].selection_region);
			retval = g_list_append (retval, selection);
		}
		
		page ++;
	}

	return retval;
}

void
ev_pixbuf_cache_reload_page (EvPixbufCache *pixbuf_cache,
			     GdkRegion     *region,
			     gint           page,
			     gint           rotation,
			     gdouble        scale)
{
	CacheJobInfo *job_info;
        gint width, height;

	job_info = find_job_cache (pixbuf_cache, page);
	if (job_info == NULL)
		return;

	_get_page_size_for_scale_and_rotation (pixbuf_cache->document,
					       page, scale, rotation,
					       &width, &height);
        add_job (pixbuf_cache, job_info, region,
		 width, height, page, rotation, scale,
		 EV_JOB_PRIORITY_URGENT);
}


