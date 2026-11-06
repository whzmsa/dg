#include <cmark.h>
#include <dirent.h>
#include <errno.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

int convert_file(const char *input_path, const char *output_path) {
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

  char *html = cmark_render_html(document, CMARK_OPT_DEFAULT);
  cmark_node_free(document);

  if (html == NULL) {
    fprintf(stderr, "Could not render %s\n", input_path);
    return 1;
  }

  FILE *output_file = fopen(output_path, "w");
  if (output_file == NULL) {
    perror(output_path);
    free(html);
    return 1;
  }

  int write_failed = (fputs(html, output_file) == EOF);

  if (fclose(output_file) == EOF)
    write_failed = 1;

  free(html);

  if (write_failed) {
    fprintf(stderr, "Could not write %s\n", output_path);
    return 1;
  }

  return 0;
}

int convert_directory(const char *input, const char *output) {
  DIR *d = opendir(input);
  if (d == NULL) {
    printf("Cannot open folder: %s\n", input);
    return 1;
  }

  int status = 0;

  if (mkdir(output, 0755) == -1) {
    if (errno != EEXIST) {
      perror(output);
      return 1;
    }

    struct stat info;

    if (stat(output, &info) == -1) {
      perror(output);
      return 1;
    }

    if (!S_ISDIR(info.st_mode)) {
      fprintf(stderr, "%s is not a directory\n", output);
      return 1;
    }
  }

  struct stat output_info;

  if (stat(output, &output_info) == -1) {
    perror(output);
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

      if (convert_directory(input_path, child_output) != 0) {
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

    if (convert_file(input_path, output_path) != 0) {
      status = 1;
    }
  }

  closedir(d);

  return status;
}

int main(int argc, char *argv[]) {
  const char *input;
  const char *output;
  if (argc < 2) {
    printf("No directory specified. Using \".\"\n");
    input = ".";
  } else {
    input = argv[1];
  }
  // if output arg not specified use output
  if (argc != 3) {
    printf("No output directory specified. Using \"./output\"\n");
    output = "output";
  } else {
    output = argv[2];
  }

  return convert_directory(input, output);
}
