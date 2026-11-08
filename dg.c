#include <cmark.h>
#include <dirent.h>
#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

typedef struct {
  char input_root[4096];
  char output_root[4096];
  bool no_index;
} Build;

static int is_inside(const char *root, const char *path) {
  size_t length = strlen(root);
  return strcmp(root, "/") == 0 ||
         (strncmp(root, path, length) == 0 &&
          (path[length] == '/' || path[length] == '\0'));
}

/* Normalize an absolute filesystem path without requiring it to exist. */
static int normalize_path(const char *path, char result[4096]) {
  char copy[4096];
  if (strlen(path) >= sizeof(copy) || path[0] != '/')
    return 1;
  strcpy(copy, path);
  strcpy(result, "/");
  char *save;
  for (char *part = strtok_r(copy, "/", &save); part;
       part = strtok_r(NULL, "/", &save)) {
    if (strcmp(part, ".") == 0)
      continue;
    if (strcmp(part, "..") == 0) {
      char *slash = strrchr(result, '/');
      if (slash != result)
        *slash = '\0';
      else
        result[1] = '\0';
      continue;
    }
    size_t used = strlen(result);
    int written =
        snprintf(result + used, 4096 - used, "%s%s", used > 1 ? "/" : "", part);
    if (written < 0 || (size_t)written >= 4096 - used)
      return 1;
  }
  return 0;
}

static int ensure_directory(const char *path) {
  char copy[4096];
  if (strlen(path) >= sizeof(copy))
    return 1;
  strcpy(copy, path);
  for (char *cursor = copy + 1;; cursor++) {
    if (*cursor != '/' && *cursor != '\0')
      continue;
    char saved = *cursor;
    *cursor = '\0';
    struct stat info;
    if ((mkdir(copy, 0755) == -1 && errno != EEXIST) ||
        stat(copy, &info) == -1 || !S_ISDIR(info.st_mode)) {
      fprintf(stderr, "Cannot create directory: %s\n", copy);
      return 1;
    }
    *cursor = saved;
    if (saved == '\0')
      return 0;
  }
}

static int copy_asset(const char *source, const char *destination) {
  char parent[4096];
  strcpy(parent, destination);
  *strrchr(parent, '/') = '\0';
  if (ensure_directory(parent) != 0)
    return 1;
  struct stat source_info, destination_info;
  if (stat(source, &source_info) == -1)
    return 1;
  if (stat(destination, &destination_info) == 0 &&
      source_info.st_dev == destination_info.st_dev &&
      source_info.st_ino == destination_info.st_ino) {
    fprintf(stderr, "Source and destination are the same: %s\n", source);
    return 1;
  }
  FILE *input = fopen(source, "rb");
  if (!input) {
    perror(source);
    return 1;
  }
  FILE *output = fopen(destination, "wb");
  if (!output) {
    perror(destination);
    fclose(input);
    return 1;
  }
  char buffer[16384];
  size_t count;
  int failed = 0;
  while ((count = fread(buffer, 1, sizeof(buffer), input)) > 0) {
    if (fwrite(buffer, 1, count, output) != count) {
      failed = 1;
      break;
    }
  }
  if (ferror(input))
    failed = 1;
  fclose(input);
  if (fclose(output) == EOF)
    failed = 1;
  if (failed)
    fprintf(stderr, "Could not copy %s to %s\n", source, destination);
  return failed;
}

static int hex_value(unsigned char value) {
  if (value >= '0' && value <= '9')
    return value - '0';
  if (value >= 'a' && value <= 'f')
    return value - 'a' + 10;
  if (value >= 'A' && value <= 'F')
    return value - 'A' + 10;
  return -1;
}

static int process_link(cmark_node *node, const char *input_path,
                        const char *output_path, const Build *build) {
  const char *url = cmark_node_get_url(node);
  size_t path_length = strcspn(url, "?#");
  /* Schemes, root-relative URLs, and fragment-only links are not files. */
  if (!path_length || url[0] == '/' || memchr(url, ':', path_length))
    return 0;
  char decoded[4096];
  size_t length = 0;
  for (size_t i = 0; i < path_length; i++) {
    unsigned char value = (unsigned char)url[i];
    if (value == '%') {
      if (i + 2 >= path_length)
        return 1;
      int high = hex_value((unsigned char)url[i + 1]);
      int low = hex_value((unsigned char)url[i + 2]);
      if (high < 0 || low < 0)
        return 1;
      value = (unsigned char)(high * 16 + low);
      i += 2;
    }
    if (!value || length + 1 >= sizeof(decoded))
      return 1;
    decoded[length++] = (char)value;
  }
  decoded[length] = '\0';

  char joined[8192], source[4096], destination[4096], resolved[4096];
  size_t parent_length = (size_t)(strrchr(input_path, '/') - input_path);
  snprintf(joined, sizeof(joined), "%.*s/%s", (int)parent_length, input_path,
           decoded);
  if (normalize_path(joined, source) || !is_inside(build->input_root, source) ||
      !realpath(source, resolved) || !is_inside(build->input_root, resolved)) {
    fprintf(stderr, "%s: missing or outside-input link: %s\n", input_path, url);
    return 1;
  }
  struct stat info;
  if (stat(source, &info) == -1 || !S_ISREG(info.st_mode)) {
    fprintf(stderr, "%s: link is not a regular file: %s\n", input_path, url);
    return 1;
  }
  if (is_inside(build->output_root, resolved)) {
    fprintf(stderr, "%s: link points into generated output: %s\n", input_path,
            url);
    return 1;
  }

  if (length >= 3 && strcmp(decoded + length - 3, ".md") == 0) {
    /* Preserve the original URL encoding, query, and fragment. */
    size_t encoded_stem = path_length;
    for (int characters = 0; characters < 3; characters++) {
      if (encoded_stem >= 3 && url[encoded_stem - 3] == '%')
        encoded_stem -= 3;
      else
        encoded_stem--;
    }
    size_t size = encoded_stem + 5 + strlen(url + path_length) + 1;
    char *rewritten = malloc(size);
    if (!rewritten)
      return 1;
    snprintf(rewritten, size, "%.*s.html%s", (int)encoded_stem, url,
             url + path_length);
    int failed = !cmark_node_set_url(node, rewritten);
    free(rewritten);
    return failed;
  }

  parent_length = (size_t)(strrchr(output_path, '/') - output_path);
  snprintf(joined, sizeof(joined), "%.*s/%s", (int)parent_length, output_path,
           decoded);
  if (normalize_path(joined, destination) ||
      !is_inside(build->output_root, destination)) {
    fprintf(stderr, "Link would copy outside output: %s\n", url);
    return 1;
  }
  return copy_asset(source, destination);
}

static int process_links(cmark_node *document, const char *input_path,
                         const char *output_path, const Build *build) {
  cmark_iter *iterator = cmark_iter_new(document);
  if (!iterator)
    return 1;
  int failed = 0;
  cmark_event_type event;
  while ((event = cmark_iter_next(iterator)) != CMARK_EVENT_DONE) {
    if (event != CMARK_EVENT_ENTER)
      continue;
    cmark_node *node = cmark_iter_get_node(iterator);
    cmark_node_type type = cmark_node_get_type(node);
    if ((type == CMARK_NODE_LINK || type == CMARK_NODE_IMAGE) &&
        process_link(node, input_path, output_path, build) != 0)
      failed = 1;
  }
  cmark_iter_free(iterator);
  return failed;
}

char *read_text_file(const char *path) {
  FILE *file = fopen(path, "rb");
  if (file == NULL) {
    perror(path);
    return NULL;
  }

  if (fseek(file, 0, SEEK_END) != 0) {
    perror(path);
    fclose(file);
    return NULL;
  }

  long file_size = ftell(file);
  if (file_size < 0) {
    perror(path);
    fclose(file);
    return NULL;
  }

  rewind(file);

  char *text = malloc((size_t)file_size + 1);
  if (text == NULL) {
    fprintf(stderr, "Could not allocate memory for %s\n", path);
    fclose(file);
    return NULL;
  }

  size_t bytes_read = fread(text, 1, (size_t)file_size, file);

  if (ferror(file)) {
    fprintf(stderr, "Could not read %s\n", path);
    free(text);
    fclose(file);
    return NULL;
  }

  text[bytes_read] = '\0';

  fclose(file);
  return text;
}

cmark_node *find_title_heading(cmark_node *node) {
  if (cmark_node_get_type(node) == CMARK_NODE_HEADING &&
      cmark_node_get_heading_level(node) == 1) {
    return node;
  }

  for (cmark_node *child = cmark_node_first_child(node); child != NULL;
       child = cmark_node_next(child)) {
    cmark_node *found = find_title_heading(child);

    if (found != NULL)
      return found;
  }
  return NULL;
}

int write_escaped(FILE *file, const char *text, size_t length) {
  for (size_t i = 0; i < length; i++) {
    const char *replacement = NULL;

    switch (text[i]) {
    case '&':
      replacement = "&amp;";
      break;
    case '<':
      replacement = "&lt;";
      break;
    case '>':
      replacement = "&gt;";
      break;
    }

    if (replacement != NULL) {
      if (fputs(replacement, file) == EOF)
        return 1;
    } else if (fputc((unsigned char)text[i], file) == EOF) {
      return 1;
    }
  }

  return 0;
}

int write_heading_text(FILE *file, cmark_node *node) {
  cmark_node_type type = cmark_node_get_type(node);

  if (type == CMARK_NODE_TEXT || type == CMARK_NODE_CODE) {
    const char *text = cmark_node_get_literal(node);
    return write_escaped(file, text, strlen(text));
  }

  if (type == CMARK_NODE_SOFTBREAK || type == CMARK_NODE_LINEBREAK) {
    return fputc(' ', file) == EOF;
  }

  for (cmark_node *child = cmark_node_first_child(node); child != NULL;
       child = cmark_node_next(child)) {
    if (write_heading_text(file, child) != 0)
      return 1;
  }

  return 0;
}

int convert_file(const char *input_path, const char *output_path,
                 const char *css, const Build *build) {
  FILE *markdown_file = fopen(input_path, "r");
  if (markdown_file == NULL) {
    perror(input_path);
    return 1;
  }

  cmark_node *document = cmark_parse_file(markdown_file, CMARK_OPT_DEFAULT);

  int read_failed = ferror(markdown_file);
  fclose(markdown_file);

  if (read_failed || document == NULL) {
    fprintf(stderr, "Could not read or parse %s\n", input_path);
    if (document != NULL)
      cmark_node_free(document);
    return 1;
  }

  int links_failed = process_links(document, input_path, output_path, build);
  char *html = cmark_render_html(document, CMARK_OPT_DEFAULT);

  if (html == NULL) {
    fprintf(stderr, "Could not render %s\n", input_path);
    cmark_node_free(document);

    return 1;
  }

  FILE *output_file = fopen(output_path, "w");
  if (output_file == NULL) {
    perror(output_path);

    free(html);
    cmark_node_free(document);

    return 1;
  }

  int write_failed =
      fprintf(output_file, "<!doctype html>\n"
                           "<html lang=\"en\">\n"
                           "<head>\n"
                           "<meta charset=\"utf-8\">\n"
                           "<meta name=\"viewport\" "
                           "content=\"width=device-width, initial-scale=1\">\n"
                           "<title>") < 0;

  cmark_node *heading = find_title_heading(document);

  if (heading != NULL) {
    if (write_heading_text(output_file, heading) != 0)
      write_failed = 1;
  } else {
    const char *filename = strrchr(input_path, '/');
    filename = filename != NULL ? filename + 1 : input_path;
    size_t length = strlen(filename);

    if (length >= 3 && strcmp(filename + length - 3, ".md") == 0)
      length -= 3;

    if (write_escaped(output_file, filename, length) != 0)
      write_failed = 1;
  }

  if (css != NULL) {
    if (fprintf(output_file,
                "</title>\n"
                "<style>\n%s\n</style>\n"
                "</head>\n"
                "<body>\n"
                "<main>\n%s\n</main>\n"
                "</body>\n"
                "</html>\n",
                css, html) < 0) {
      write_failed = 1;
    }
  } else {
    if (fprintf(output_file,
                "</title>\n"
                "</head>\n"
                "<body>\n"
                "<main>\n%s\n</main>\n"
                "</body>\n"
                "</html>\n",
                html) < 0) {
      write_failed = 1;
    }
  }

  cmark_node_free(document);

  if (fclose(output_file) == EOF)
    write_failed = 1;

  free(html);

  if (write_failed) {
    fprintf(stderr, "Could not write %s\n", output_path);
    return 1;
  }

  return links_failed;
}

int convert_directory(const char *input, const char *output, const char *css,
                      const Build *build) {
  DIR *d = opendir(input);
  if (d == NULL) {
    printf("Cannot open folder: %s\n", input);
    return 1;
  }

  int status = 0;
  int created_output = 0;

  if (mkdir(output, 0755) == 0) {
    created_output = 1;
  } else if (errno != EEXIST) {
    perror(output);
    closedir(d);
    return 1;
  }

  struct stat output_info;

  if (stat(output, &output_info) == -1) {
    perror(output);
    closedir(d);
    return 1;
  }

  if (!S_ISDIR(output_info.st_mode)) {
    fprintf(stderr, "%s is not a directory\n", output);
    closedir(d);
    return 1;
  }
  struct dirent *e;
  // loop all files in input dir
  while ((e = readdir(d)) != NULL) {
    // skip the current and parent dirs
    // strcmp = string compare. compares directory name with . or .. to filter
    // them out.
    if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) {
      continue;
    }

    // build the full input path
    char input_path[4096];
    int written =
        snprintf(input_path, sizeof(input_path), "%s/%s", input, e->d_name);

    if (written < 0 || (size_t)written >= sizeof(input_path)) {
      fprintf(stderr, "Input path is too long: %s\n", e->d_name);
      continue;
    }

    // get stat of file. basically stats i guess
    struct stat info;

    if (lstat(input_path, &info) == -1) {
      perror(input_path);
      status = 1;
      continue;
    }

    char resolved_entry[4096];
    if (realpath(input_path, resolved_entry) &&
        is_inside(build->output_root, resolved_entry))
      continue;

    if (info.st_dev == output_info.st_dev &&
        info.st_ino == output_info.st_ino) {
      continue;
    }

    // check if entry is directory
    if (S_ISDIR(info.st_mode)) {
      char child_output[4096];

      int written = snprintf(child_output, sizeof(child_output), "%s/%s",
                             output, e->d_name);

      if (written < 0 || (size_t)written >= sizeof(child_output)) {
        fprintf(stderr, "Output path is too long: %s\n", e->d_name);
        status = 1;
        continue;
      }

      if (convert_directory(input_path, child_output, css, build) != 0) {
        status = 1;
      }
      continue;
    }

    // check if entry is regular file
    if (!S_ISREG(info.st_mode)) {
      continue;
    }

    // check if d_name has .md extension
    size_t length = strlen(e->d_name);
    if (length < 3 || strcmp(e->d_name + length - 3, ".md") != 0) {
      continue;
    }

    // puts = print variable to console (ig)
    // puts(e->d_name);

    char output_path[4096];

    // TODO: fix typo
    int writte_output =
        snprintf(output_path, sizeof(output_path), "%s/%.*s.html", output,
                 (int)(length - 3), e->d_name);

    if (writte_output < 0 || (size_t)writte_output >= sizeof(output_path)) {
      fprintf(stderr, "Output path is too long: %s\n", e->d_name);
      continue;
    }

    printf("%s -> %s\n", input_path, output_path);

    // put input_path in console
    // puts(input_path);

    if (convert_file(input_path, output_path, css, build) != 0) {
      status = 1;
    } else if (!build->no_index && strcmp(e->d_name, "README.md") == 0) {
        char index_path[4096];

        int written = snprintf(index_path, sizeof(index_path), "%s/index.html", output);

        if (writte_output < 0 || (size_t)written >=sizeof(index_path)) {
            fprintf(stderr, "Index path is too long\n");
            status = 1;
        } else if (copy_asset(output_path, index_path) != 0) {
            status = 1;
        }
    }
  }

  closedir(d);

  if (created_output) {
    if (rmdir(output) == -1) {
      if (errno != ENOTEMPTY && errno != EEXIST) {
        perror(output);
        status = 1;
      }
    }
  }

  return status;
}

static void print_usage(FILE *stream, const char *program) {
  fprintf(stream,
          "Usage: %s [INPUT_DIR [OUTPUT_DIR [CSS_FILE]]]\n"
          "       %s -h | --help\n"
          "\n"
          "Generate HTML documentation from Markdown files recursively.\n"
          "\n"
          "Arguments:\n"
          "  INPUT_DIR   Directory containing Markdown files (default: .)\n"
          "  OUTPUT_DIR  Directory for generated HTML (default: output)\n"
          "  CSS_FILE    Optional stylesheet to embed in each HTML page\n"
          "\n"
          "Options:\n"
          "  -h, --help  Show this help and exit\n"
          "  --no-index  Disable copying README.md to index.html and README.html\n"
          "\n"
          "Relative paths are resolved from the current working directory.\n"
          "\n"
          "Examples:\n"
          "  %s\n"
          "  %s docs public\n"
          "  %s docs public marx.min.css\n",
          program, program, program, program, program);
}

int main(int argc, char *argv[]) {
  bool no_index = false;
  const char *paths[3];
  int path_count = 0;

  for (int i = 1; i < argc; i++) {
    if (strcmp(argv[i], "--no-index") == 0) {
      no_index = true;
    } else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
      print_usage(stdout, argv[0]);
      return 0;
    } else if (argv[i][0] == '-') {
      fprintf(stderr, "Unknown option: %s\n", argv[i]);
      return 1;
    } else {
      if (path_count == 3) {
        fprintf(stderr, "Too many directory or file arguments.\n");
        return 1;
      }
      paths[path_count++] = argv[i];
    }
  }

  const char *input = path_count >= 1 ? paths[0] : ".";
  const char *output = path_count >= 2 ? paths[1] : "output";
  char *css = NULL;

  if (path_count >= 3) {
    css = read_text_file(paths[2]);
    if (css == NULL)
      return 1;
  }

  Build build;
  build.no_index = no_index;
  char cwd[4096], joined[8192];
  if (!realpath(input, build.input_root) || !getcwd(cwd, sizeof(cwd))) {
    perror(input);
    free(css);
    return 1;
  }
  snprintf(joined, sizeof(joined), "%s/%s", output[0] == '/' ? "" : cwd,
           output);
  if (normalize_path(joined, build.output_root) ||
      is_inside(build.output_root, build.input_root)) {
    fprintf(stderr, "Output must not equal or contain the input directory\n");
    free(css);
    return 1;
  }
  if (ensure_directory(build.output_root) != 0 ||
      !realpath(build.output_root, joined)) {
    free(css);
    return 1;
  }
  strcpy(build.output_root, joined);
  if (is_inside(build.output_root, build.input_root)) {
    fprintf(stderr, "Output must not equal or contain the input directory\n");
    free(css);
    return 1;
  }
  int status =
      convert_directory(build.input_root, build.output_root, css, &build);

  free(css);
  return status;
}
