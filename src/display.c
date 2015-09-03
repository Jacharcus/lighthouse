/** @file display.c
 *  @author Bram Wasti <bwasti@cmu.edu>
 *  
 *  @brief This file contains the logic that draws to the screen.
 */

#include <stdio.h>
#include <unistd.h>
#include <wordexp.h>

#include "display.h"
#include "globals.h"

#define min(a,b) ((a) < (b) ? (a) : (b))

/* @brief Type used to pass around x,y offsets. */
typedef struct {
  uint32_t x;
  uint32_t y;
  uint32_t image_y;
} offset_t;

/* @brief Returns the offset for a line of text.
 *
 * @param line the index of the line to be drawn (counting from the top).
 * @return the line's offset.
 */
static inline offset_t calculate_line_offset(uint32_t line) {
  offset_t result;
  result.x  = settings.horiz_padding;
  result.y  = settings.height * line;
  result.image_y = result.y;
  result.y +=  + global.real_font_size;
  /* To draw a picture cairo need to know the top left corner
   * position. But to draw a text cairo need to know the bottom
   * left corner position.
   */

  return result;
}

/* @brief Resize an image.
 *
 * @param *surface The image to resize.
 * @param width The width of the current image.
 * @param height The height of the current image.
 * @param new_width The width of the image when resized.
 * @param new_height The height of the image when resized.
 */
cairo_surface_t * scale_surface (cairo_surface_t *surface, int width, int height,
        int new_width, int new_height) {
  cairo_surface_t *new_surface = cairo_surface_create_similar(surface,
          CAIRO_CONTENT_COLOR_ALPHA, new_width, new_height);
  cairo_t *cr = cairo_create (new_surface);

  cairo_scale (cr, (double)new_width / width, (double)new_height / height);
  cairo_set_source_surface (cr, surface, 0, 0);

  cairo_pattern_set_extend (cairo_get_source(cr), CAIRO_EXTEND_REFLECT);
  cairo_set_operator (cr, CAIRO_OPERATOR_SOURCE);

  cairo_paint (cr);

  cairo_destroy (cr);

  return new_surface;
}

/* @brief Draw a line of text with a cursor to a cairo context.
 *
 * @param cr A cairo context for drawing to the screen.
 * @param text The text to be drawn.
 * @param line The index of the line to be drawn (counting from the top).
 * @param cursor The index of the cursor into the text to be drawn.
 * @param foreground The color of the text.
 * @param background The color of the background.
 * @return Void.
 */
static void draw_typed_line(cairo_t *cr, char *text, uint32_t line, uint32_t cursor, color_t *foreground, color_t *background) {
  pthread_mutex_lock(&global.draw_mutex);

  /* Set the background. */
  cairo_set_source_rgb(cr, background->r, background->g, background->b);
  cairo_rectangle(cr, 0, line * settings.height, settings.width, (line + 1) * settings.height);
  cairo_stroke_preserve(cr);
  cairo_fill(cr);

  /* Set the foreground color and font. */
  cairo_set_source_rgb(cr, foreground->r, foreground->g, foreground->b);
  cairo_select_font_face(cr, settings.font_name, CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);

  offset_t offset = calculate_line_offset(line);
  /* Find the cursor relative to the text. */
  cairo_text_extents_t extents;
  char saved_char = text[cursor];
  text[cursor] = '\0';
  cairo_text_extents(cr, text, &extents);
  text[cursor] = saved_char;
  int32_t cursor_x = extents.x_advance;

  /* Find the text offset. */
  cairo_text_extents(cr, text, &extents);
  if (settings.width < extents.width) {
    offset.x = settings.width - extents.x_advance;
  }

  cursor_x += offset.x;

  /* if the cursor would be off the back end, set its position to 0 and scroll text instead */
  if(cursor_x < 0) {
      offset.x -= (cursor_x-3);
      cursor_x = 0;
  }

  /* Draw the text. */
  cairo_move_to(cr, offset.x, offset.y);
  cairo_set_font_size(cr, settings.font_size);
  cairo_show_text(cr, text);

  /* Draw the cursor. */
  if (settings.cursor_is_underline) {
    cairo_show_text(cr, "_");
  } else {
    uint32_t cursor_y = offset.y - settings.font_size - settings.cursor_padding;
    cairo_set_source_rgb(cr, foreground->r, foreground->g, foreground->b);
    cairo_rectangle(cr, cursor_x + 2, cursor_y, 0, settings.font_size + (settings.cursor_padding * 2));
    cairo_stroke_preserve(cr);
    cairo_fill(cr);
  }

  pthread_mutex_unlock(&global.draw_mutex);
}

/* @brief Draw text at the given offset.
 *
 * @param cr A cairo context for drawing to the screen.
 * @param text The text to be drawn.
 * @param foreground The color of the text.
 * @return The advance in the x direction.
 */
static uint32_t draw_text(cairo_t *cr, const char *text, offset_t offset, color_t *foreground, cairo_font_weight_t weight) {
  cairo_text_extents_t extents;
  cairo_text_extents(cr, text, &extents);
  cairo_move_to(cr, offset.x, offset.y);
  cairo_set_source_rgb(cr, foreground->r, foreground->g, foreground->b);
  cairo_select_font_face(cr, settings.font_name, CAIRO_FONT_SLANT_NORMAL, weight);
  cairo_set_font_size(cr, settings.font_size);
  cairo_show_text(cr, text);
  return extents.x_advance;
}

/* @brief Draw an image at the given offset.
 *
 * @param cr A cairo context for drawing to the screen.
 * @param file The image to be drawn.
 * @param Current offset in the line/desc, used to know the image position.
 * @param win_size_x Width of the window.
 * @param win_size_y Height of the window.
 * @return The advance in the x direction.
 */
static image_format_t draw_image(cairo_t *cr, const char *file, offset_t offset, uint32_t win_size_x, uint32_t win_size_y) {
  wordexp_t expanded_file;
  image_format_t format;

  if (wordexp(file, &expanded_file, 0)) {
    fprintf(stderr, "Error expanding file %s\n", file);
  } else {
    file = expanded_file.we_wordv[0];
  }

  if (access(file, F_OK) == -1) {
    fprintf(stderr, "Cannot open image file %s\n", file);
    format.width = 0;
    format.height = 0;
    return format;
  }

  cairo_surface_t *img;
  img = cairo_image_surface_create_from_png(file);
  format.width = cairo_image_surface_get_width(img);
  format.height = cairo_image_surface_get_height(img);

  if (format.width > win_size_x || format.height > win_size_y) {
      /* Formatting only the big picture. */
      float prop = min((float)win_size_x / format.width,
              (float)win_size_y / format.height);
      /* Finding the best proportion to fit the picture. */
      image_format_t new_format;
      new_format.width = prop * format.width;
      new_format.height = prop * format.height;

      img = scale_surface(img, format.width, format.height,
              new_format.width, new_format.height);
      format = new_format;
      debug("Resizing the image to %ix%i (prop = %f)\n", format.width, format.height, prop);
  }

  debug("Drawing the picture in x:%i, y:%i\n", offset.x, offset.image_y);
  cairo_set_source_surface(cr, img, offset.x, offset.image_y);
  cairo_mask_surface(cr, img, offset.x, offset.image_y);

  return format;
}

/* @brief Draw a line of text to a cairo context.
 *
 * @param cr A cairo context for drawing to the screen.
 * @param text The text to be drawn.
 * @param line The index of the line to be drawn (counting from the top).
 * @param foreground The color of the text.
 * @param background The color of the background.
 * @return Void.
 */
static void draw_line(cairo_t *cr, const char *text, uint32_t line, color_t *foreground, color_t *background) {
  pthread_mutex_lock(&global.draw_mutex);

  cairo_set_source_rgb(cr, background->r, background->g, background->b);
  /* Add 2 offset to height to prevent flickery drawing over the typed text.
   * TODO: Use better math all around. */
  cairo_rectangle(cr, 0, line * settings.height + 2, settings.width, (line + 1) * settings.height);
  cairo_stroke_preserve(cr);
  cairo_fill(cr);
  offset_t offset = calculate_line_offset(line);

  /* Parse the result line as we draw it. */
  char *c = (char *)text;
  while (c && *c != '\0') {
    draw_t d = parse_result_line(cr, &c, settings.width - offset.x);
    if (d.data == NULL)
        break;
    /* Checking if there are still char to draw. */

    char saved = *c;
    *c = '\0';
    switch (d.type) {
      case DRAW_IMAGE: ;
        image_format_t format;
        format = draw_image(cr, d.data, offset, settings.width - offset.x, settings.height);
        offset.x += format.width;
        break;
      case BOLD:
        offset.x += draw_text(cr, d.data, offset, foreground, CAIRO_FONT_WEIGHT_BOLD);
        break;
      case NEW_LINE:
        break;
      case DRAW_TEXT:
      default:
        offset.x += draw_text(cr, d.data, offset, foreground, CAIRO_FONT_WEIGHT_NORMAL);
        break;
    }
    *c = saved;
  }

  pthread_mutex_unlock(&global.draw_mutex);
}

/* @brief Draw a description to a cairo context.
 *
 * @param cr A cairo context for drawing to the screen.
 * @param text The description to be drawn.
 * @param foreground The color of the text.
 * @param background The color of the background.
 * @return Void.
 */
static void draw_desc(cairo_t *cr, const char *text, color_t *foreground, color_t *background) {
  pthread_mutex_lock(&global.draw_mutex);
  cairo_set_source_rgb(cr, background->r, background->g, background->b);
  uint32_t desc_height = settings.height*(global.result_count+1);
  cairo_rectangle(cr, settings.width + 2, 0,
          settings.width+settings.desc_size, desc_height);
  cairo_stroke_preserve(cr);
  cairo_fill(cr);
  offset_t offset = {settings.width + 2, global.real_font_size, 0};

  /* Parse the result line as we draw it. */
  char *c = (char *)text;
  while (c && *c != '\0') {
    draw_t d = parse_result_line(cr, &c, settings.desc_size + settings.width - offset.x);
    char saved = *c;
    *c = '\0';
    switch (d.type) {
      case DRAW_IMAGE: ;
        image_format_t format;
        format = draw_image(cr, d.data, offset, settings.desc_size, desc_height - offset.image_y);
        offset.image_y += format.height;
        offset.y = offset.image_y;
        offset.x += format.width;
        /* We set the offset.y and x next to the picture so the user can choose to
         * return to the next line or not.
         */
        break;
      case NEW_LINE:
        offset.x = settings.width;
        offset.y += settings.font_size;
        offset.image_y += settings.font_size;
        break;
      case BOLD:
        offset.x += draw_text(cr, d.data, offset, foreground, 1);
        break;
      case DRAW_TEXT:
      default:
        offset.x += draw_text(cr, d.data, offset, foreground, 0);
        break;
    }
    *c = saved;
    if ((offset.x + settings.font_size) > (settings.width + settings.desc_size)) {
        /* Checking if it's gonna write out of the square space. */
        offset.x = settings.width;
        offset.y += global.real_font_size;
        offset.image_y += global.real_font_size;
    }
  }

  pthread_mutex_unlock(&global.draw_mutex);
}

void draw_query_text(cairo_t *cr, cairo_surface_t *surface, const char *text, uint32_t cursor) {
  draw_typed_line(cr, (char *)text, 0, cursor, &settings.query_fg, &settings.query_bg);
  cairo_surface_flush(surface);
}

void draw_result_text(xcb_connection_t *connection, xcb_window_t window, cairo_t *cr, cairo_surface_t *surface, result_t *results) {
  int32_t line, index;
  if (global.result_count - 1 < global.result_highlight) {
    global.result_highlight = global.result_count - 1;
  }

  uint32_t max_results = settings.max_height / settings.height - 1;
  uint32_t display_results = min(global.result_count, max_results);
  if ((global.result_offset + display_results) < (global.result_highlight + 1)) {
    global.result_offset = global.result_highlight - (display_results - 1);
    display_results = global.result_count - global.result_offset;
  } else if (global.result_offset > global.result_highlight) {
    global.result_offset = global.result_highlight;
  }

  if ((global.result_highlight < global.result_count) &&
          results[global.result_highlight].desc) {
      if (settings.auto_center) {
        uint32_t values[] = { global.win_x_pos_with_desc, global.win_y_pos };
        xcb_configure_window(connection, window, XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y, values);
      }

      uint32_t new_height = min(settings.height * (global.result_count + 1), settings.max_height);
      uint32_t values[] = { settings.width+settings.desc_size, new_height };
      xcb_configure_window(connection, window, XCB_CONFIG_WINDOW_WIDTH | XCB_CONFIG_WINDOW_HEIGHT, values);
      cairo_xcb_surface_set_size(surface, settings.width + settings.desc_size, new_height);
      draw_desc(cr, results[global.result_highlight].desc, &settings.highlight_fg, &settings.highlight_bg);
  } else {
      if (settings.auto_center) {
        uint32_t values[] = { global.win_x_pos, global.win_y_pos };
        xcb_configure_window(connection, window, XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y, values);
      }

      uint32_t new_height = min(settings.height * (global.result_count + 1), settings.max_height);
      uint32_t values[] = { settings.width, new_height };
      xcb_configure_window (connection, window, XCB_CONFIG_WINDOW_WIDTH | XCB_CONFIG_WINDOW_HEIGHT, values);
      cairo_xcb_surface_set_size(surface, settings.width, new_height);
  }

  for (index = global.result_offset, line = 1; index < global.result_offset + display_results; index++, line++) {
    if (index != global.result_highlight) {
      draw_line(cr, results[index].text, line, &settings.result_fg, &settings.result_bg);
    } else {
      draw_line(cr, results[index].text, line, &settings.highlight_fg, &settings.highlight_bg);
    }
  }
  cairo_surface_flush(surface);
  xcb_flush(connection);
}

void redraw_all(xcb_connection_t *connection, xcb_window_t window, cairo_t *cr, cairo_surface_t *surface, char *query_string, uint32_t query_cursor_index) {
  draw_query_text(cr, surface, query_string, query_cursor_index);
  draw_result_text(connection, window, cr, surface, global.results);
}
