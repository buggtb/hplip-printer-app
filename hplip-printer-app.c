//
// HPLIP (hpcups) Printer Application based on PAPPL and libpappl-retrofit
//
// Copyright © 2020-2021 by Till Kamppeter.
// Copyright © 2020 by Michael R Sweet.
//
// Licensed under Apache License v2.0.  See the file "LICENSE" for more
// information.
//

//
// Include necessary headers...
//

#define _GNU_SOURCE

#include <pappl-retrofit.h>
#include <curl/curl.h>
#include <sys/stat.h>
#include <openssl/sha.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <sys/types.h>
#include <dirent.h>
#include <pthread.h>
#include <fcntl.h>
#include "hplip-plugin-verify.h"

#include "hplip-download-policy.h"

//
// Constants...
//

// Name and version

#define SYSTEM_NAME "HPLIP Printer Application"
#define SYSTEM_PACKAGE_NAME "hplip-printer-app"
#ifndef SYSTEM_VERSION_STR
#  define SYSTEM_VERSION_STR "1.0"
#endif
#ifndef SYSTEM_VERSION_ARR_0
#  define SYSTEM_VERSION_ARR_0 1
#endif
#ifndef SYSTEM_VERSION_ARR_1
#  define SYSTEM_VERSION_ARR_1 0
#endif
#ifndef SYSTEM_VERSION_ARR_2
#  define SYSTEM_VERSION_ARR_2 0
#endif
#ifndef SYSTEM_VERSION_ARR_3
#  define SYSTEM_VERSION_ARR_3 0
#endif
#define SYSTEM_WEB_IF_FOOTER "Copyright &copy; 2021 by Till Kamppeter. Provided under the terms of the <a href=\"https://www.apache.org/licenses/LICENSE-2.0\">Apache License 2.0</a>."

// Test page

#define TESTPAGE "testpage.pdf"

// System architecture

#if defined(__x86_64__) || defined(_M_X64)
  #define ARCH "x86_64"
#elif defined(i386) || defined(__i386__) || defined(__i386) || defined(_M_IX86)
  #define ARCH "x86_32"
#elif defined(__aarch64__) || defined(_M_ARM64) || defined(__ARM_ARCH_ISA_A64)
  #define ARCH "arm64"
#elif defined(__arm__) || defined(_M_ARM)
  #define ARCH "arm32"
#else
  // "grep"ping the binary should not match this
  #define ARCH "UNSUPPORTED_ARCH"
#endif

#ifndef HPLIP_OCI
#  define HPLIP_OCI 0
#endif

#define HPLIP_CAN_INSTALL_PLUGIN (HPLIP_OCI || !getuid())

// Plugin download URLs

#define PLUGIN_CONF_URL "https://hplip.sf.net/plugin.conf"
#define PLUGIN_ALT_LOCATION "https://developers.hp.com/sites/default/files"

// Helper which installs, removes, and recovers the proprietary plugin with
// a durable transaction, so that a restart in the middle of an upgrade
// does not lose a working installation. See
// "scripts/hplip-plugin-state.sh" for what it does and why.

#ifndef HPLIP_PLUGIN_STATE_SCRIPT
#  define HPLIP_PLUGIN_STATE_SCRIPT "/usr/share/hplip-printer-app/hplip-plugin-state.sh"
#endif

//
// Global state...
//

// Serializes HP plugin install/update operations. Two web-admin plugin
// installation requests must not operate on the same plugin_tmp staging
// directory and plugin state at the same time. The busy check in the web
// handler uses the non-blocking pthread_mutex_trylock() and reports a busy
// state instead of overlapping the mutation. The startup auto-update uses
// the blocking pthread_mutex_lock().
static pthread_mutex_t plugin_install_mutex = PTHREAD_MUTEX_INITIALIZER;


//
// Types...
//

typedef enum hplip_plugin_status_e
{
  HPLIP_PLUGIN_NOT_INSTALLED = 0,
  HPLIP_PLUGIN_OUTDATED,
  HPLIP_PLUGIN_INSTALLED
} hplip_plugin_status_t;


//
// Functions...
//

//
// 'get_config_value()' - Return the value of a given variable/key in
//                        a given section of a config file
//

char *
get_config_value(FILE *fp,
		 const char *section,
		 const char *key)
{
  char line[1024];
  char *value = NULL;
  int in_section = 0;

  if (!key || !key[0])
    return (NULL);

  rewind(fp);

  while (fgets(line, sizeof(line), fp))
  {
    while (line[strlen(line) - 1] == '\n' ||
	   line[strlen(line) - 1] == '\r') // Remove newline
      line[strlen(line) - 1] = '\0';

    if (line[0] == '[')
    {
      if (section && !strncasecmp(line + 1, section, strlen(section)) &&
	  line[strlen(section) + 1] == ']')
	in_section = 1;
      else
	in_section = 0;
    }
    else if ((!section || in_section) &&
	     !strncasecmp(line, key, strlen(key)))
    {
      value = line + strlen(key);
      while (*value && isspace(*value)) value ++;
      if (*value == '=')
      {
	value ++;
	while (*value && isspace(*value)) value ++;
	if (*value)
	{
	  value = strdup(value);
	  break;
	}
	else
	  value = NULL;
      }
      else
	value = NULL;
    }
  }

  return (value);
}


//
// 'hplip_version()' - Read out the HPLIP version from /etc/hp/hplip.conf
//

char *
hplip_version(pappl_system_t *system)
{
  char buf[1024];
  FILE *fp;
  char *version = NULL;


  snprintf(buf, sizeof(buf), "%s/%s", HPLIP_CONF_DIR, "hplip.conf");
  if ((fp = fopen(buf, "r")) == NULL)
  {
    papplLog(system, PAPPL_LOGLEVEL_ERROR,
	     "Unable to open HPLIP configuration file %s", buf);
    return (NULL);
  }

  version = get_config_value(fp, "hplip", "version");

  fclose(fp);

  return (version);
}


//
// 'hplip_plugin_status()' - Read the installed plugin's version from state.
//

hplip_plugin_status_t
hplip_plugin_status(pappl_system_t *system)
{
  char buf[1024];
  FILE *fp;
  char *plugin_version,
       *installed_status,
       *version;
  hplip_plugin_status_t status = HPLIP_PLUGIN_NOT_INSTALLED;


  snprintf(buf, sizeof(buf), "%s/%s", HPLIP_PLUGIN_STATE_DIR, "hplip.state");
  if ((fp = fopen(buf, "r")) == NULL)
  {
    if (errno != ENOENT)
      papplLog(system, PAPPL_LOGLEVEL_ERROR,
	       "Unable to open HPLIP plugin status file %s: %s",
	       buf, strerror(errno));
    return (status);
  }


  plugin_version = get_config_value(fp, "plugin", "version");
  installed_status = get_config_value(fp, "plugin", "installed");

  if (installed_status && atoi(installed_status) != 0 &&
      plugin_version && plugin_version[0] &&
      (version = hplip_version(system)) != NULL)
  {
    if (!strcasecmp(plugin_version, version))
      status = HPLIP_PLUGIN_INSTALLED;
    else
      status = HPLIP_PLUGIN_OUTDATED;
    free(version);
  }

  if (plugin_version)
    free(plugin_version);
  if (installed_status)
    free(installed_status);
  fclose(fp);

  return (status);
}


//
// 'hplip_download_file() - Download a file from a given URL to a local
//                          temporary file
//
// Every transfer is bounded by connect, total, and stall timeouts, so a
// server which accepts the connection but never answers cannot hold the
// web admin request which triggered the download open forever.  On
// failure the caller gets NULL, a sentence describing what went wrong
// in 'error', and no file left behind.
//

char*
hplip_download_file(pappl_system_t *system, const char *url,
		    char *error, size_t error_size)
{
  int           fd;
  char          tempfile[1024] = "";
  CURL *curl;
  FILE *fp = NULL;
  CURLcode ret = CURLE_OK;
  hplip_download_policy_t policy;
  double        connect_time = 0.0,
		total_time = 0.0;


  if (error && error_size)
    error[0] = '\0';

  // Work out the bounds for this transfer
  hplip_download_policy_defaults(&policy);
  hplip_download_policy_from_env(&policy);
  if (policy.rejected)
    papplLog(system, PAPPL_LOGLEVEL_ERROR,
	     "Ignored %d unusable plugin download bound setting(s), the built-in values are in use. See the %s, %s, %s and %s environment variables.",
	     policy.rejected,
	     HPLIP_DOWNLOAD_CONNECT_TIMEOUT_ENV, HPLIP_DOWNLOAD_TOTAL_TIMEOUT_ENV,
	     HPLIP_DOWNLOAD_STALL_LIMIT_ENV, HPLIP_DOWNLOAD_STALL_TIME_ENV);

  papplLog(system, PAPPL_LOGLEVEL_DEBUG,
	   "Downloading %s (connect timeout %lds, total timeout %lds, abort after %lds below %ld bytes/s)",
	   url, policy.connect_timeout, policy.total_timeout,
	   policy.stall_time, policy.stall_limit);

  // Setup curl
  curl = curl_easy_init();
  if (curl)
  {
    // Create a temporary file
    fd = cupsTempFd(tempfile, sizeof(tempfile));
    if (fd < 0 || !tempfile[0])
    {
      papplLog(system, PAPPL_LOGLEVEL_ERROR,
	       "Unable to create temporary file");
      curl_easy_cleanup(curl);
      return (NULL);
    }

    if ((fp = fdopen(fd, "wb")) == NULL)
    {
      // Without a stream to write to, libcurl would be handed a NULL
      // FILE and the download would never be attempted.
      papplLog(system, PAPPL_LOGLEVEL_ERROR,
	       "Unable to open temporary file %s: %s",
	       tempfile, strerror(errno));
      close(fd);
      unlink(tempfile);
      curl_easy_cleanup(curl);
      return (NULL);
    }

#if HPLIP_OCI
    // The index and plugin must use authenticated transport in the OCI image.
    curl_easy_setopt(curl, CURLOPT_PROTOCOLS_STR, "https");
    curl_easy_setopt(curl, CURLOPT_REDIR_PROTOCOLS_STR, "https");
#endif
    curl_easy_setopt(curl, CURLOPT_FAILONERROR, 1L);
    // Download the file
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, fwrite);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, fp);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 50L);
    hplip_download_policy_apply(curl, &policy);
    ret = curl_easy_perform(curl);

    // Ask libcurl what actually happened before the handle goes away:
    // which of the timeouts expired is not in the result code.
    curl_easy_getinfo(curl, CURLINFO_CONNECT_TIME, &connect_time);
    curl_easy_getinfo(curl, CURLINFO_TOTAL_TIME, &total_time);

    curl_easy_cleanup(curl);
    fclose(fp);
  }
  else
  {
    papplLog(system, PAPPL_LOGLEVEL_ERROR,
	     "Unable to start curl");
    return (NULL);
  }

  // Check for errors
  if (ret == CURLE_OK)
    return(strdup(tempfile));
  else
  {
    // Describe the failure for the web interface, and drop the partial
    // file so that nothing which was cut short can be used later.
    hplip_download_policy_message(ret, &policy, connect_time > 0.0,
				  total_time, error, error_size);

    papplLog(system, PAPPL_LOGLEVEL_DEBUG,
	     "Download status: %d (%s)", ret, curl_easy_strerror(ret));
    papplLog(system, PAPPL_LOGLEVEL_ERROR,
	     "Unable to download file %s: %s", url,
	     (error && error[0]) ? error : curl_easy_strerror(ret));
    unlink(tempfile);
  }

  return (NULL);
}


//
// 'hplip_run_command_line()' - Run a command line and log its screen output,
//                              both stdout and stderr. Return the exit code
//                              of the command.
//

int
hplip_run_command_line(pappl_system_t *system, const char *command)
{
  FILE *fp;
  char buf[1024];


  if ((fp = popen(command, "r")) == NULL)
  {
    papplLog(system, PAPPL_LOGLEVEL_ERROR,
	     "Unable to start \"gpg\" utility fo plugin signature verification. Command: %s",
	     command);
    return (-1);
  }
  papplLog(system, PAPPL_LOGLEVEL_DEBUG,
	   "Executing command: %s", command);

  while (fgets(buf, sizeof(buf), fp) != NULL)
  {
    buf[strlen(buf) - 1] = '\0'; // Remove newline
    papplLog(system, PAPPL_LOGLEVEL_DEBUG, "  %s", buf);
  }

  return (pclose(fp));
}


//
// 'hplip_is_safe_argument_char()' - Check whether a character can be part
//                                   of an argument for the plugin state
//                                   helper.
//

int
hplip_is_safe_argument_char(int c)
{
  // Everything which can never be a shell metacharacter, a quote, or a
  // line break. All values passed to the helper are the HPLIP plugin
  // directory, the directory of its state file, and a HPLIP version
  // number.
  return ((c >= '0' && c <= '9') ||
	  (c >= 'A' && c <= 'Z') ||
	  (c >= 'a' && c <= 'z') ||
	  c == '/' || c == '.' || c == '_' || c == '-' || c == '+');
}


//
// 'hplip_quote_argument()' - Put an argument into single quotes, so that
//                            it cannot become part of the command itself.
//                            Returns 0 if the argument cannot be passed.
//

int
hplip_quote_argument(char *buf, size_t bufsize, const char *arg,
		     pappl_system_t *system)
{
  const char *a;


  if (!arg || !arg[0])
  {
    papplLog(system, PAPPL_LOGLEVEL_ERROR,
	     "Empty argument for the HPLIP plugin state helper.");
    return (0);
  }

  for (a = arg; *a; a ++)
  {
    if (!hplip_is_safe_argument_char(*a & 255))
    {
      papplLog(system, PAPPL_LOGLEVEL_ERROR,
	       "Refusing to pass \"%s\" to the HPLIP plugin state helper.",
	       arg);
      return (0);
    }
  }

  if (strlen(arg) + 3 > bufsize)
  {
    papplLog(system, PAPPL_LOGLEVEL_ERROR,
	     "Argument \"%s\" is too long for the HPLIP plugin state helper.",
	     arg);
    return (0);
  }

  snprintf(buf, bufsize, "'%s'", arg);

  return (1);
}


//
// 'hplip_run_plugin_state()' - Install, remove, or recover the HPLIP
//                              plugin with a durable transaction, by
//                              calling "scripts/hplip-plugin-state.sh".
//                              "command" is one of the literals "install",
//                              "remove", and "recover", it never comes
//                              from a configuration file or from the
//                              network.
//

int
hplip_run_plugin_state(pappl_system_t *system, const char *command,
		       const char *plugin_dir, const char *version)
{
  char buf[4096], quoted_dir[1024], quoted_state[1024], quoted_version[256];
  int ret;


  if (!hplip_quote_argument(quoted_dir, sizeof(quoted_dir), plugin_dir,
			    system) ||
      !hplip_quote_argument(quoted_state, sizeof(quoted_state),
			    HPLIP_PLUGIN_STATE_DIR, system))
    return (0);

  if (version)
  {
    if (!hplip_quote_argument(quoted_version, sizeof(quoted_version),
			      version, system))
      return (0);

    snprintf(buf, sizeof(buf), "%s %s %s %s %s",
	     HPLIP_PLUGIN_STATE_SCRIPT, command, quoted_dir, quoted_state,
	     quoted_version);
  }
  else
    snprintf(buf, sizeof(buf), "%s %s %s %s",
	     HPLIP_PLUGIN_STATE_SCRIPT, command, quoted_dir, quoted_state);

  if ((ret = hplip_run_command_line(system, buf)) != 0)
  {
    papplLog(system, PAPPL_LOGLEVEL_ERROR,
	     "The HPLIP plugin state helper failed (status %d) for the command \"%s\".",
	     ret, command);
    return (0);
  }

  return (1);
}


//
// 'hplip_get_uncompress_dir() - Determine (and create) the directory
//                               where the plugin file should be
//                               uncompressed after download
//

char *
hplip_get_uncompress_dir(pappl_system_t *system, int create)
{
#ifdef HPLIP_PLUGIN_ALT_DIR

  (void)system;
  (void)create;

  return strdup(HPLIP_PLUGIN_ALT_DIR);

#else

  char buf[1024],
       *home;


  if ((home = getenv("HOME")) == NULL)
  {
    papplLog(system, PAPPL_LOGLEVEL_ERROR,
	     "No HOME environment variable defined.");
    return (NULL);
  }

  snprintf(buf, sizeof(buf), "%s/.hplip", home);
  papplLog(system, PAPPL_LOGLEVEL_DEBUG,
	   "Directory to uncompress the plugin in: %s", buf);

  if (create)
  {
    papplLog(system, PAPPL_LOGLEVEL_DEBUG,
	     "Creating directory for uncompressed plugin %s", buf);
    if (mkdir(buf, S_IRWXU) == -1)
    {
      if (errno != EEXIST)
      {
	papplLog(system, PAPPL_LOGLEVEL_ERROR,
		 "Error creating directory %s: %s", buf, strerror(errno));
	return (NULL);
      }
    }
  }

  return (strdup(buf));

#endif // HPLIP_PLUGIN_ALT_DIR
}


//
// 'hplip_remove_uncompress_dir() - Remove the uncompressed plugin
//                                  file when we do not need it any more
//

int
hplip_remove_uncompress_dir(pappl_system_t *system, const char *name)
{
  char *uncompress_dir;
  char buf[1024];
  DIR *d;
  struct dirent *entry;
  char *filename;
  int len;


  // Find the directory where the uncompressed plugin is located
  if ((uncompress_dir = hplip_get_uncompress_dir(system, 0)) == NULL)
  {
    papplLog(system, PAPPL_LOGLEVEL_ERROR,
	     "Could not determine the directory with the uncompressed plugin file.");
    return (0);
  }

  // Open the directory with the files of the uncompressed plugin
  len = snprintf(buf, sizeof(buf), "%s/%s", uncompress_dir, name);
  if ((d = opendir(buf)) == NULL && errno != ENOENT)
  {
    papplLog(system, PAPPL_LOGLEVEL_ERROR,
	     "Could not open the directory %s: %s", buf, strerror(errno));
    free(uncompress_dir);
    return (0);
  }

  // Remove the files of the plugin (only regular files and symlinks, no
  // sub-directories)
  if (d)
  {
    papplLog(system, PAPPL_LOGLEVEL_DEBUG,
	     "Removing uncompressed plugin directory %s", buf);
    while (entry = readdir(d))
    {
      if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, ".."))
	continue;
      snprintf(buf + len, sizeof(buf) - len, "/%s", entry->d_name);
      if (unlink(buf) != 0)
	papplLog(system, PAPPL_LOGLEVEL_ERROR,
		 "Could not remove file %s: %s", buf, strerror(errno));
    }
    closedir(d);

    // Remove the emptied directory
    buf[len] = '\0';
    if (rmdir(buf) != 0)
    {
      papplLog(system, PAPPL_LOGLEVEL_ERROR,
	       "Could not remove directory %s: %s", buf, strerror(errno));
      free(uncompress_dir);
      return (0);
    }
  }

#ifndef HPLIP_PLUGIN_ALT_DIR

  // Try to remove the directory in which we had uncompressed the
  // plugin, but ignore errors (for example if it contains something
  // else, then we most probably did not create it in the first place)
  rmdir(uncompress_dir);
  if (errno == 0)
    papplLog(system, PAPPL_LOGLEVEL_DEBUG,
	     "Removed directory for uncompressed plugin %s", uncompress_dir);

#endif // !HPLIP_PLUGIN_ALT_DIR

  free(uncompress_dir);
  return (1);
}


//
// 'hplip_download_plugin()' - Download the plugin from the HP's official
//                             locations, validate, and uncompress it
//
// On failure the caller gets NULL and, in 'error', a sentence naming
// the step which failed and why, for the web interface to show.
//

char *
hplip_download_plugin(pappl_system_t *system,
		      char *error, size_t error_size)
{
  int i;
  char *plugin_conf = NULL,
       *plugin_file = NULL,
       *signature_file = NULL,
       *version = NULL,
       *url = NULL,
       *size_str = NULL,
       *checksum = NULL,
       *uncompress_dir = NULL,
       *ret = NULL;
  FILE *fp = NULL;
  size_t plugin_size;
  char buf[1024];
  char reason[512];
  struct stat st;
  int fd;
  int bytes;
  SHA_CTX ctx;
  unsigned char hash[SHA_DIGEST_LENGTH];
  int status;
  const char *state_dir;


  if (error && error_size)
    error[0] = '\0';

  // Get plugin index file from HP
  papplLog(system, PAPPL_LOGLEVEL_DEBUG,
	   "Getting plugin index from HP ...");
  if ((plugin_conf = hplip_download_file(system, PLUGIN_CONF_URL,
					 reason, sizeof(reason))) == NULL)
  {
    papplLog(system, PAPPL_LOGLEVEL_ERROR,
	     "Unable to download plugin index");
    if (error && error_size)
      snprintf(error, error_size,
	       "the plugin index from HP could not be retrieved: %s", reason);
    goto out;
  }

  // HPLIP version which we have installed, equals section name in the plugin
  // index
  if ((version = hplip_version(system)) == NULL)
  {
    papplLog(system, PAPPL_LOGLEVEL_ERROR,
	     "Unable to determine version of HPLIP");
    goto out;
  }

  // Open downloaded plugin index
  if ((fp = fopen(plugin_conf, "r")) == NULL)
  {
    papplLog(system, PAPPL_LOGLEVEL_ERROR,
	     "Unable to open downloaded plugin index file");
    goto out;
  }

  // Read needed data items: URL, size, checksum
  if ((url = get_config_value(fp, version, "url")) == NULL || !url[0])
  {
    papplLog(system, PAPPL_LOGLEVEL_ERROR,
	     "Could not find plugin URL in index file. Is HPLIP %s already released?",
	     version);
    goto out;
  }

  if ((size_str = get_config_value(fp, version, "size")) == NULL ||
      !size_str[0] || (plugin_size = atoi(size_str)) <= 0)
  {
    papplLog(system, PAPPL_LOGLEVEL_ERROR,
	     "Could not find plugin size in index file.");
    goto out;
  }

  if ((checksum = get_config_value(fp, version, "checksum")) == NULL ||
      !checksum[0])
  {
    papplLog(system, PAPPL_LOGLEVEL_ERROR,
	     "Could not find plugin checksum in index file.");
    goto out;
  }

  fclose(fp);
  fp = NULL;
#if HPLIP_OCI
  // HP's index still advertises HTTP for older releases. Upgrade its known
  // OpenPrinting mirror before downloading, and reject other plaintext URLs.
  if (!strncasecmp(url, "http://www.openprinting.org/",
                   sizeof("http://www.openprinting.org/") - 1))
  {
    char *secure_url = NULL;
    if (asprintf(&secure_url, "https://%s", url + 7) < 0)
      goto out;
    free(url);
    url = secure_url;
  }
#endif

  // Download the plugin file
  buf[0] = '\0';
  papplLog(system, PAPPL_LOGLEVEL_DEBUG,
	   "Getting plugin file for HPLIP %s from official location ...",
	   version);
  papplLog(system, PAPPL_LOGLEVEL_DEBUG,
	   "Trying URL: %s", url);
  if ((plugin_file = hplip_download_file(system, url,
					 reason, sizeof(reason))) == NULL)
  {
    papplLog(system, PAPPL_LOGLEVEL_DEBUG,
	     "Unable to download plugin file, trying backup server");
    snprintf(buf, sizeof(buf) - 5, "%s/hplip-%s-plugin.run",
	     PLUGIN_ALT_LOCATION, version);
    papplLog(system, PAPPL_LOGLEVEL_DEBUG,
	     "Trying URL: %s", buf);
    if ((plugin_file = hplip_download_file(system, buf,
					   reason, sizeof(reason))) == NULL)
    {
      papplLog(system, PAPPL_LOGLEVEL_ERROR,
	     "Unable to download plugin file.");
      if (error && error_size)
	snprintf(error, error_size,
		 "the plugin file could not be retrieved from HP or from its backup location: %s",
		 reason);
      goto out;
    }
  }

  // Download the plugin signature file
  papplLog(system, PAPPL_LOGLEVEL_DEBUG,
	   "Getting plugin signature file from the same location as the plugin file ...");
  if (buf[0])
    strcpy(buf + strlen(buf), ".asc");
  else
    snprintf(buf, sizeof(buf), "%s.asc", url);
  papplLog(system, PAPPL_LOGLEVEL_DEBUG,
	   "Using URL: %s", buf);
  if ((signature_file = hplip_download_file(system, buf,
					    reason, sizeof(reason))) == NULL)
  {
    papplLog(system, PAPPL_LOGLEVEL_ERROR,
	     "Unable to download plugin signature file.");
    if (error && error_size)
      snprintf(error, error_size,
	       "the plugin signature file could not be retrieved: %s", reason);
    goto out;
  }

  // Determine size of the downloaded plugin
  if (stat(plugin_file, &st) != 0)
    goto out;
  if (st.st_size != plugin_size)
  {
    papplLog(system, PAPPL_LOGLEVEL_ERROR,
	     "Downloaded plugin file is not of expected size. File has %ld bytes but expected are %ld bytes.",
	     st.st_size, plugin_size);
    goto out;
  }
  else
    papplLog(system, PAPPL_LOGLEVEL_DEBUG,
	     "Downloaded file size OK (%ld).", st.st_size);

  // Verify sha1sum of the plugin file against the checksum from the index file
  SHA1_Init(&ctx);
  fd = open(plugin_file, O_RDONLY);
  if (fd < 0)
    goto out;
  while ((bytes = read(fd, buf, sizeof(buf))) > 0)
    SHA1_Update(&ctx, buf, bytes);
  close(fd);
  if (bytes < 0)
    goto out;
  SHA1_Final(hash, &ctx);
  for (i = 0; i < SHA_DIGEST_LENGTH; i ++)
    snprintf(buf + 2 * i, 3, "%.2x", hash[i]);
  buf[2 * SHA_DIGEST_LENGTH] = '\0';
  if (strcasecmp(buf, checksum))
  {
    papplLog(system, PAPPL_LOGLEVEL_ERROR,
	     "Checksum of the plugin file (%s) does not match checksum of the plugin index (%s).",
	     buf, checksum);
    goto out;
  }
  else
    papplLog(system, PAPPL_LOGLEVEL_DEBUG,
	     "Downloaded file checksum OK (%s).", buf);

  // Only the packaged HP key may authorize executing the downloaded archive.
  // Keep GPG state private and persistent, separate from the user's keyring.
  state_dir = getenv("STATE_DIR");
  if (!state_dir || !*state_dir)
    state_dir = HPLIP_APP_STATE_DIR;
  if (snprintf(buf, sizeof(buf), "%s/plugin-gnupg", state_dir) >= sizeof(buf) ||
      !hplip_verify_plugin(buf, HPLIP_SIGNING_KEY,
                           "4ABA2F66DBD5A95894910E0673D770CDA59047B9",
                           signature_file, plugin_file))
  {
    papplLog(system, PAPPL_LOGLEVEL_ERROR,
             "Plugin signature verification failed; no plugin was extracted.");
    goto out;
  }

  // Get the directory where to uncompress the plugin
  if ((uncompress_dir = hplip_get_uncompress_dir(system, 1)) == NULL)
  {
    papplLog(system, PAPPL_LOGLEVEL_ERROR,
	     "Could not find/create the directory for uncompressing the plugin.");
    goto out;
  }

  // Make sure we have the directory name "plugin_tmp" free for uncompressing
  // our plugin
  if (!hplip_remove_uncompress_dir(system, "plugin_tmp"))
  {
    papplLog(system, PAPPL_LOGLEVEL_ERROR,
	     "Old uncompressed plugin file is in the way: %s/plugin_tmp",
	     uncompress_dir);
    goto out;
  }

  // Uncompress the plugin
  snprintf(buf, sizeof(buf),
	   "cd %s 2>&1 && mkdir plugin_tmp 2>&1 && cd plugin_tmp 2>&1 && sh %s --tar xf --no-same-owner 2>&1 && cd .. 2>&1 && chmod -R go+rX plugin_tmp 2>&1",
	   uncompress_dir, plugin_file);
  papplLog(system, PAPPL_LOGLEVEL_DEBUG,
	   "Uncompressing the plugin file.");

  if ((status = hplip_run_command_line(system, buf)) != 0)
  {
    papplLog(system, PAPPL_LOGLEVEL_ERROR,
	     "Unable to uncompress the plugin");
    goto out;
  }

  papplLog(system, PAPPL_LOGLEVEL_DEBUG,
	   "Downloaded, verified, and uncompressed the plugin successfully. Ready to install in %s/plugin_tmp",
	   uncompress_dir);

  // Output the uncompress location
  ret = uncompress_dir;

 out:

  // Anything which failed after the downloads - a rejected signature, a
  // size or checksum mismatch, an unusable staging directory - still
  // needs to say something to the user, so never hand back an empty
  // reason.
  if (ret == NULL && error && error_size && !error[0])
    snprintf(error, error_size,
	     "the plugin could not be verified or prepared, see the log for details");

  // Clean up
  if (fp)
    fclose(fp);
  if (plugin_conf)
  {
    unlink(plugin_conf);
    free(plugin_conf);
  }
  if (signature_file)
  {
    unlink(signature_file);
    free(signature_file);
  }
  if (version)
    free(version);
  if (url)
    free(url);
  if (size_str)
    free(size_str);
  if (checksum)
    free(checksum);
  if (plugin_file)
  {
    unlink(plugin_file);
    free(plugin_file);
  }
  if (ret == NULL && uncompress_dir)
  {
    hplip_remove_uncompress_dir(system, "plugin_tmp");
    free(uncompress_dir);
  }

  return (ret);
}


//
// 'hplip_install_plugin() - Install the downloaded plugin, after the
//                           license got accepted in the web
//                           interface, or right after uncompressing
//                           during an update. Classic installations use
//                           installPlugin.py; Snap and OCI rename the verified
//                           plugin_tmp directory into persistent plugin state
//                           and link the current architecture's libraries.
//                           Snap needs root, while OCI uses its writable volume.
//

int
hplip_install_plugin(pappl_system_t *system, const char *plugin_dir)
{
  int ret = 0;

#if defined(SNAP) || HPLIP_OCI

  int len;
  char buf1[1024], buf2[1024];
  DIR *d;
  struct dirent *entry;
  struct stat st;
  char *p, *version = NULL;
  int arch_matches = 0;

  // Open the directory with the files of the uncompressed plugin
  len = snprintf(buf1, sizeof(buf1), "%s/plugin_tmp", plugin_dir);
  if ((d = opendir(buf1)) == NULL)
  {
    papplLog(system, PAPPL_LOGLEVEL_ERROR,
	     "Could not open the directory %s: %s", buf1, strerror(errno));
    goto out;
  }

  // Go through all the files of the plugin, and for the dynamic link
  // libraries (*.so files) link the ones of our system's architecture.
  // HPLIP's vendor archive tags files as "<component>-<ARCH>.so[.version]"
  // where ARCH is one of "x86_32", "x86_64", "arm32", "arm64" (the same
  // labels this Printer Application uses, see the ARCH macro above), not
  // OCI/Debian-style "amd64"/"arm64" triplets. If a vendor bundle ever
  // uses different aliases for our architecture, or simply lacks any
  // files for it (this has historically been true for some
  // ARM-only-partial vendor bundles), zero symlinks get created and the
  // plugin silently ends up non-functional; make that failure loud
  // instead of quietly continuing as if the plugin were fully installed.
  papplLog(system, PAPPL_LOGLEVEL_DEBUG,
	   "Adding symlinks for dynamic link libraries of the %s architecture in %s", ARCH, buf1);
  buf1[len] = '/';
  len ++;
  while (entry = readdir(d))
  {
    if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, ".."))
      continue;
    if ((p = strstr(entry->d_name, ARCH)) != NULL &&
	p > entry->d_name && *(p - 1) == '-' &&
	strncmp(p + strlen(ARCH), ".so", 3) == 0)
    {
      memmove(buf1 + len, entry->d_name, (p - entry->d_name) - 1);
      memmove(buf1 + len + (p - entry->d_name) - 1, p + strlen(ARCH),
	      strlen(p + strlen(ARCH)) + 1);
      if (symlink(entry->d_name, buf1) != 0)
      {
	papplLog(system, PAPPL_LOGLEVEL_ERROR,
		 "Could not create symboiic link %s to %s: %s",
		 buf1, entry->d_name, strerror(errno));
	closedir(d);
	goto out;
      }
      arch_matches ++;
    }
  }
  closedir(d);

  if (arch_matches == 0)
  {
    papplLog(system, PAPPL_LOGLEVEL_ERROR,
	     "No plugin library files matching architecture \"%s\" were found "
	     "in %s/plugin_tmp; the downloaded vendor plugin does not support "
	     "this architecture, refusing to report the plugin as installed",
	     ARCH, plugin_dir);
    goto out;
  }

  // Resolve the version before touching a working installation.
  if ((version = hplip_version(system)) == NULL)
  {
    papplLog(system, PAPPL_LOGLEVEL_ERROR,
             "Unable to determine version of HPLIP");
    goto out;
  }

  snprintf(buf2, sizeof(buf2), "%s/plugin", plugin_dir);
  if (lstat(buf2, &st) == 0 && !S_ISDIR(st.st_mode))
  {
    papplLog(system, PAPPL_LOGLEVEL_ERROR,
             "Existing plugin is not a directory: %s", buf2);
    goto out;
  }

  // Make the downloaded and verified plugin the installed one and register
  // it. This is a durable transaction which keeps the previously installed
  // plugin until the new one is in place and registered, so that a restart
  // in the middle of it neither loses a working installation nor leaves a
  // plugin directory and a registered version which do not belong
  // together. An interrupted transaction gets completed by
  // hplip_recover_plugin() during the next start.
  papplLog(system, PAPPL_LOGLEVEL_DEBUG,
	   "Installing the plugin (version %s) in %s", version, plugin_dir);

  if (!hplip_run_plugin_state(system, "install", plugin_dir, version))
  {
    papplLog(system, PAPPL_LOGLEVEL_ERROR,
	     "Unable to install the HPLIP plugin into %s.", plugin_dir);
    goto out;
  }

#else

  char buf[1024];

  // Run HP's script (included in the plugin) to install the plugin
  snprintf(buf, sizeof(buf),
	   "cd %s/plugin_tmp 2>&1 && python3 installPlugin.py 2>&1",
	   plugin_dir);
  papplLog(system, PAPPL_LOGLEVEL_DEBUG,
	   "Installing the plugin.");

  if (hplip_run_command_line(system, buf) != 0)
  {
    papplLog(system, PAPPL_LOGLEVEL_ERROR,
	     "Unable to install the plugin");
    goto out;
  }

#endif // SNAP || HPLIP_OCI

  // Done
  papplLog(system, PAPPL_LOGLEVEL_DEBUG,
	   "Installed the plugin successfully.");
  ret = 1;

 out:
#if defined(SNAP) || HPLIP_OCI
  free(version);
#endif
  // Remove the uncompressed plugin file
  hplip_remove_uncompress_dir(system, "plugin_tmp");

  return (ret);
}


#if defined(SNAP) || HPLIP_OCI
//
// 'hplip_remove_plugin()' - Uninstall a Snap/OCI plugin from its persistent
//                           state and unregister its installation status.
//                           Uses the same durable transaction as
//                           hplip_install_plugin(), so that a restart in the
//                           middle of a removal cannot leave an unregistered
//                           plugin directory behind.
//

int
hplip_remove_plugin(pappl_system_t *system, const char *plugin_dir)
{
  int ret = 0;


  papplLog(system, PAPPL_LOGLEVEL_DEBUG,
	   "Removing plugin directory %s/plugin",
	   plugin_dir);

  if (!hplip_run_plugin_state(system, "remove", plugin_dir, NULL))
  {
    papplLog(system, PAPPL_LOGLEVEL_ERROR,
	     "Unable to remove the HPLIP plugin from %s.", plugin_dir);
    goto out;
  }

  // Done
  papplLog(system, PAPPL_LOGLEVEL_DEBUG,
	   "Uninstalled the plugin successfully.");
  ret = 1;

 out:

  return (ret);
}


//
// 'hplip_recover_plugin()' - Complete or undo a plugin installation or
//                            removal which an abrupt restart interrupted
//                            (Snap and OCI).
//                            Needs to run before the plugin status gets
//                            read for the first time, so that the web
//                            interface and the automatic update report
//                            the version of the plugin which is really
//                            installed.
//

void
hplip_recover_plugin(pappl_system_t *system)
{
  char *plugin_dir;


  if ((plugin_dir = hplip_get_uncompress_dir(system, 0)) == NULL)
  {
    papplLog(system, PAPPL_LOGLEVEL_ERROR,
	     "Unable to determine the directory of the plugin for its recovery.");
    return;
  }

  if (!hplip_run_plugin_state(system, "recover", plugin_dir, NULL))
    papplLog(system, PAPPL_LOGLEVEL_ERROR,
	     "Unable to recover an interrupted installation or removal of the HPLIP plugin. Please install the plugin again in the web interface.");

  free(plugin_dir);
}
#endif // SNAP || HPLIP_OCI


//
// 'hplip_web_plugin()' - Web interface page for installing, updating,
//                        and removing HP's proprietary plugin.
//

void
hplip_web_plugin(
    pappl_client_t *client,		// I - Client
    void *data)                         // I - Global data
{
  pr_printer_app_global_data_t *global_data =
    (pr_printer_app_global_data_t *)data;
  pappl_system_t      *system = prGetSystem(global_data); // System
  const char          *status = NULL;	// Status text, if any
  const char          *uri = NULL;      // Client URI
  pappl_version_t     version;
  hplip_plugin_status_t plugin_status;
  char                *plugin_dir = NULL;
  char                buf[2048];
  char                errmsg[512];	// Why a download failed
  char                download_status[1024]; // Status line including that
  char                *licensetext = NULL;
  int                 plugin_locked = 0;
  FILE                *fp;
  size_t              size_needed;


  if (!papplClientHTMLAuthorize(client))
    return;

  errmsg[0] = '\0';
  download_status[0] = '\0';

  // Get status of installed plugin
  plugin_status = hplip_plugin_status(system);

  // Handle POSTs to add and delete PPD files...
  if (papplClientGetMethod(client) == HTTP_STATE_POST)
  {
    int			num_form = 0;	// Number of form variables
    cups_option_t	*form = NULL;	// Form variables
    const char		*action;	// Form action

    if ((num_form = papplClientGetForm(client, &form)) == 0)
    {
      status = "Invalid form data.";
    }
    else if (!papplClientIsValidForm(client, num_form, form))
    {
      status = "Invalid form submission.";
    }
    else if ((action = cupsGetOption("action", num_form, form)) == NULL)
    {
      status = "Missing action.";
    }
    else if (!strcmp(action, "install-plugin") ||
	     !strcmp(action, "install-plugin-yes") ||
	     !strcmp(action, "license-accepted"))
    {
      // Serialize plugin install/update: only one request may mutate the
      // plugin_tmp staging directory and the plugin state at a time. If
      // another request already holds the lock, report a busy state instead
      // of overlapping the mutation (acceptance criteria #1).
      plugin_locked = 1;
      if (pthread_mutex_trylock(&plugin_install_mutex) != 0)
      {
	plugin_locked = 0;
	status = "A plugin installation is already running. Please wait a moment and try again.";
      }
      else
      {
	// Set status to trigger the "Are you sure?" page when we
	// re-install over an already installed and up-to-date plugin
	status = "Installing plugin";
      }
      if (plugin_locked && strcmp(action, "license-accepted") &&
	  (plugin_status != HPLIP_PLUGIN_INSTALLED ||
	   !strcmp(action, "install-plugin-yes")))
      {
	// Download the plugin
	// Snap needs root; the OCI appliance installs only into its user-owned volume.
	if (HPLIP_CAN_INSTALL_PLUGIN)
        {
	  papplLog(system, PAPPL_LOGLEVEL_DEBUG,
		   "Downloading the proprietary plugin ...");
	  if ((plugin_dir = hplip_download_plugin(system, errmsg,
						  sizeof(errmsg))) == NULL)
	    papplLog(system, PAPPL_LOGLEVEL_ERROR,
		     "Unable to download plugin: %s", errmsg);
	  else
	    papplLog(system, PAPPL_LOGLEVEL_DEBUG,
		     "Plugin downloaded to %s", plugin_dir);
	}
	else
	{
	  papplLog(system, PAPPL_LOGLEVEL_ERROR,
		   "Printer Application must run as root to download/install plugin.");
	  snprintf(errmsg, sizeof(errmsg),
		   "the Printer Application is not running as root, so it cannot install the plugin");
	}
	if (plugin_dir)
	  // Succeeded, set status so that if no installation follows now
	  // we get onto the license page (if plugin_status ==
	  // HPLIP_PLUGIN_NOT_INSTALLED)
	  status = "Plugin downloaded.";
	else
	{
	  // Failed, get back to plugin status page with the reason.  A
	  // download which ran into a bound is a bounded, explained
	  // failure rather than a request which hangs.
	  snprintf(download_status, sizeof(download_status),
		   "Plugin download failed: %s.",
		   errmsg[0] ? errmsg : "the reason is in the log file");
	  status = download_status;
	}
      }
      if (plugin_locked &&
	  (plugin_status == HPLIP_PLUGIN_OUTDATED ||
	   !strcmp(action, "install-plugin-yes") ||
	   !strcmp(action, "license-accepted")))
      {
	// Install the plugin
	// If failed, get back to plugin status page
	status = "Plugin installation failed.";
	// Snap needs root; the OCI appliance installs only into its user-owned volume.
	if (HPLIP_CAN_INSTALL_PLUGIN)
        {
	  papplLog(system, PAPPL_LOGLEVEL_DEBUG,
		   "Installing the proprietary plugin ...");
	  if (!plugin_dir)
	    plugin_dir = hplip_get_uncompress_dir(system, 0);
	  if (plugin_dir)
	  {
	    if (hplip_install_plugin(system, plugin_dir))
	    {
	      // Succeeded, update plugin status and get back to plugin
	      // status page
	      plugin_status = HPLIP_PLUGIN_INSTALLED;
	      status = "Plugin installed.";
	      papplLog(system, PAPPL_LOGLEVEL_DEBUG,
		       "Plugin installed.");
	    }
	    else
	      papplLog(system, PAPPL_LOGLEVEL_ERROR,
		       "Plugin installation failed.");
	  }
	  else
	    papplLog(system, PAPPL_LOGLEVEL_ERROR,
		     "Could not find/the directory with the downloaded plugin.");
	}
      }
    }
#if defined(SNAP) || HPLIP_OCI
    else if (!strcmp(action, "remove-plugin"))
    {
      // Only set status to trigger the "Are you sure?" page
      status = "Removing plugin.";
    }
    else if (!strcmp(action, "remove-plugin-yes"))
    {
      // Serialize plugin removal with install/update: a remove can race an
      // in-flight install that is mid-rename on the same plugin/ path, so it
      // takes the same lock as install (acceptance criteria #1).
      plugin_locked = 1;
      if (pthread_mutex_trylock(&plugin_install_mutex) != 0)
      {
        plugin_locked = 0;
        status = "A plugin installation is already running. Please wait a moment and try again.";
      }
      else
      {
      // Remove the plugin
      // If failed, get back to plugin status page
      status = "Plugin removal failed.";
      // Snap needs root; the OCI appliance modifies only its user-owned volume.
      if (HPLIP_CAN_INSTALL_PLUGIN)
      {
	papplLog(system, PAPPL_LOGLEVEL_DEBUG,
		 "Removing the proprietary plugin ...");
	if (!plugin_dir)
	  plugin_dir = hplip_get_uncompress_dir(system, 0);
	if (plugin_dir)
	{
	  if (hplip_remove_plugin(system, plugin_dir))
	  {
	    // Succeeded, update plugin status and get back to plugin
	    // status page
	    plugin_status = HPLIP_PLUGIN_NOT_INSTALLED;
	    status = "Plugin removed.";
	    papplLog(system, PAPPL_LOGLEVEL_DEBUG,
		     "Plugin removed.");
	  }
	  else
	    papplLog(system, PAPPL_LOGLEVEL_ERROR,
		     "Plugin removal failed.");
	}
	else
	  papplLog(system, PAPPL_LOGLEVEL_ERROR,
		   "Could not find/the directory with the installed plugin.");
      }
      }
    }
#endif
    else if (!strcmp(action, "license-declined"))
    {
      // License declined
      // Remove downloaded and uncompressed plugin (plugin_tmp), but never
      // while another request is installing from that staging directory.
      if (pthread_mutex_trylock(&plugin_install_mutex) != 0)
      {
	status = "A plugin installation is already running. Please wait a moment and try again.";
      }
      else
      {
	plugin_locked = 1;
	if (!plugin_dir)
	  plugin_dir = hplip_get_uncompress_dir(system, 0);
	if (plugin_dir)
	{
	  if (hplip_remove_uncompress_dir(system, "plugin_tmp") == 0)
	  {
	    papplLog(system, PAPPL_LOGLEVEL_ERROR,
		     "Unable to remove plugin directory %s/plugin_tmp",
		     plugin_dir);
	  }
	}
	// Set status to get back onto plugin status page
	status = "License declined, plugin not installed.";
      }
    }
    else if (!strcmp(action, "install-cancel"))
    {
      // "Are you sure?" on re-intall canceled
      // Only set status to get back onto plugin status page
      status = "Plugin not re-installed.";
    }
#if defined(SNAP) || HPLIP_OCI
    else if (!strcmp(action, "remove-cancel"))
    {
      // "Are you sure?" on remove canceled
      // Only set status to get back onto plugin status page
      status = "Plugin not removed.";
    }
#endif // SNAP
    else
      status = "Unknown action.";

    cupsFreeOptions(num_form, form);
  }

  // Find license file
  buf[0] = '\0';
  if (status && strcasestr(status, "downloaded") && plugin_dir)
    // Load license text from downloaded plugin
    snprintf(buf, sizeof(buf), "%s/plugin_tmp/license.txt", plugin_dir);
  else if (plugin_status != HPLIP_PLUGIN_NOT_INSTALLED)
  {
    // Load license text from installed plugin
#if defined(SNAP) || HPLIP_OCI
    if (!plugin_dir)
      plugin_dir = hplip_get_uncompress_dir(system, 0);
    snprintf(buf, sizeof(buf), "%s/plugin/license.txt", plugin_dir);
#else
    char *hplip_home = NULL;

    snprintf(buf, sizeof(buf), "%s/%s", HPLIP_CONF_DIR, "hplip.conf");
    if ((fp = fopen(buf, "r")) == NULL)
    {
      papplLog(system, PAPPL_LOGLEVEL_ERROR,
	       "Unable to open HPLIP configuration file %s", buf);
      buf[0] = '\0';
    }
    else
    {
      hplip_home = get_config_value(fp, "dirs", "home");
      fclose(fp);
      if (hplip_home)
      {
	snprintf(buf, sizeof(buf), "%s/data/plugins/license.txt", hplip_home);
	free(hplip_home);
      }
      else
      {
	papplLog(system, PAPPL_LOGLEVEL_ERROR,
		 "Unable to locate HPLIP data directory via the config file %s, cannot load license text",
		 buf);
	buf[0] = '\0';
      }
    }
#endif // SNAP
  }

  // Load license text 
  if (buf[0])
  {
    // Open license file
    papplLog(system, PAPPL_LOGLEVEL_DEBUG,
	     "Loading license text from %s", buf);
    if ((fp = fopen(buf, "r")) == NULL)
    {
      papplLog(system, PAPPL_LOGLEVEL_ERROR,
	       "Unable to open plugin license file %s: %s",
	       buf, strerror(errno));
    }
    else
    {
      // Load complete file into a buffer
      fseek(fp, 0L, SEEK_END);
      size_needed = ftell(fp);
      licensetext = (char *)calloc(size_needed + 1, sizeof(char));
      rewind(fp);
      if (fread(licensetext, 1, size_needed, fp) != size_needed)
      {
	papplLog(system, PAPPL_LOGLEVEL_ERROR,
		 "Unable to read plugin license file %s: %s",
		 buf, strerror(errno));
	free(licensetext);
	licensetext = strdup("Unable to load license file.");
	papplClientRespondRedirect(client, HTTP_STATUS_FOUND, "/plugin");
      }
      fclose(fp);
    }
  }

  // Output web interface page
  if (!papplClientRespond(client, HTTP_STATUS_OK, NULL, "text/html", 0, 0))
    goto clean_up;
  papplClientHTMLHeader(client, "Proprietary Plugin for HPLIP", 0);
  if (papplSystemGetVersions(system, 1, &version) > 0)
    papplClientHTMLPrintf(client,
                          "    <div class=\"header2\">\n"
                          "      <div class=\"row\">\n"
                          "        <div class=\"col-12 nav\">\n"
                          "          Version %s\n"
                          "        </div>\n"
                          "      </div>\n"
                          "    </div>\n", version.sversion);
  papplClientHTMLPuts(client, "    <div class=\"content\">\n");

  papplClientHTMLPrintf(client,
			"      <div class=\"row\">\n"
			"        <div class=\"col-12\">\n"
			"          <h1 class=\"title\">Proprietary Plugin for HPLIP</h1>\n");

  if (status)
    papplClientHTMLPrintf(client, "          <div class=\"banner\">%s</div>\n", status);

  if (status && strcasestr(status, "downloaded"))
  {
    // Show license text and buttons to accept and to reject installation
    // plugin_dir /plugin_tmp/license.txt

    // Display license text
    papplClientHTMLPuts(client,
			"        <p>You are about to install the proprietary plugin from HP. You have to agree with the following license to use it:\n");
    papplClientHTMLPrintf(client,
			  "        <div class=\"log\"><pre>%s</pre></div>\n",
			  licensetext);

    uri = papplClientGetURI(client);

    papplClientHTMLStartForm(client, uri, false);
    papplClientHTMLPuts(client,
			"          <table class=\"form\">\n"
			"            <tbody>\n");

    papplClientHTMLPuts(client, "          <tr><td><button type=\"submit\" name=\"action\" value=\"license-accepted\">Agree</button>");
    papplClientHTMLPuts(client, "&nbsp;<button type=\"submit\" name=\"action\" value=\"license-declined\">Decline</button>");
    papplClientHTMLPuts(client,
			"</td></tr>\n"
			"            </tbody>\n"
			"          </table>\n"
			"        </form>\n");
  }
  else if (status && strcasestr(status, "installing"))
  {
    // Ask the user whether he is sure to re-install the plugin
    papplClientHTMLPuts(client,
			"        <p>You are about to re-install the already installed plugin from HP. This is only needed if you are facing problems with the plugin.</p>\n");
    papplClientHTMLPuts(client,
			"        <p>Are you sure?</p>\n");

    uri = papplClientGetURI(client);

    papplClientHTMLStartForm(client, uri, false);
    papplClientHTMLPuts(client,
			"          <table class=\"form\">\n"
			"            <tbody>\n");

    papplClientHTMLPuts(client, "          <tr><td><button type=\"submit\" name=\"action\" value=\"install-plugin-yes\">Re-Install Plugin</button>");
    papplClientHTMLPuts(client, "&nbsp;<button type=\"submit\" name=\"action\" value=\"install-cancel\">Cancel</button>");
    papplClientHTMLPuts(client,
			"</td></tr>\n"
			"            </tbody>\n"
			"          </table>\n"
			"        </form>\n");
  }
#if defined(SNAP) || HPLIP_OCI
  else if (status && strcasestr(status, "removing"))
  {
    // Ask the user whether he is sure to remove the plugin
    papplClientHTMLPuts(client,
			"        <p>You are about to remove the installed plugin from HP. This is only needed if you are facing problems with the plugin, and if you have devices which need the plugin, they will stop working.</p>\n");
    papplClientHTMLPuts(client,
			"        <p>Are you sure?</p>\n");

    uri = papplClientGetURI(client);

    papplClientHTMLStartForm(client, uri, false);
    papplClientHTMLPuts(client,
			"          <table class=\"form\">\n"
			"            <tbody>\n");

    papplClientHTMLPuts(client, "          <tr><td><button type=\"submit\" name=\"action\" value=\"remove-plugin-yes\">Remove Plugin</button>");
    papplClientHTMLPuts(client, "&nbsp;<button type=\"submit\" name=\"action\" value=\"remove-cancel\">Cancel</button>");
    papplClientHTMLPuts(client,
			"</td></tr>\n"
			"            </tbody>\n"
			"          </table>\n"
			"        </form>\n");
  }
#endif // SNAP
  else
  {
    // Show plugin status and buttons to install, re-install, uninstall plugin

    papplClientHTMLPuts(client,
			"        <p>Some devices cannot be supported by HP's free-(open-source) software driver suite <a href=\"https://developers.hp.com/hp-linux-imaging-and-printing\">HPLIP</a> (HP Linux Imaging and Printing) but need driver extensions or firmware files which are proprietary software (binary-only, no source code published) which cannot be distributed in this Printer Application or in Linux distributions. Therefore all these pieces of proprietary software are packaged together by HP to form a single plugin package ");
    if (getuid())
      papplClientHTMLPuts(client,
			  "which can be installed with HPLIP.</p>\n");
    else
      papplClientHTMLPuts(client,
			  "which can be installed here.</p>\n");
    papplClientHTMLPuts(client,
		      "        <p>In most cases, for ~95% of the supported printers you do not need to install it. Only if the printer model you have selected on the \"Add Printer\" page is marked with \"requires proprietary plugin\" or you find this remark in the print queue entry once you have set up your printer, you need to install this plugin.</p>\n");
#if SNAP
    papplClientHTMLPuts(client,
			"        <p>With each new release of HPLIP by HP also a new version of the plugin is issued. You do not need to do any manual updates, this Snap gets auto-updated and if the update contains a new version of HPLIP, also the plugin (if it is installed) gets updated.</p>\n");
#else
    papplClientHTMLPuts(client,
			"        <p>With each new release of HPLIP by HP also a new version of the plugin is issued. You do not need to do any manual updates, this Printer Application automatically updates the plugin (if it is installed) when a new version of HPLIP gets installed on the system.</p>\n");
#endif // SNAP
    papplClientHTMLPuts(client,
			"        <h3>Plugin installation status</h3>\n");
    papplClientHTMLPrintf(client,
			  "        <p><blockquote><b style=\"font-size:200%%;\">%s</b></blockquote></p>\n",
			  plugin_status == HPLIP_PLUGIN_NOT_INSTALLED ?
			  "Plugin NOT installed" :
			  (plugin_status == HPLIP_PLUGIN_INSTALLED ?
			   "Plugin installed and up-to-date" :
			   (plugin_status == HPLIP_PLUGIN_OUTDATED ?
			    "Plugin out-of-date" :
			    "Plugin status unknown")));

    if (getuid())
    {
      if (plugin_status != HPLIP_PLUGIN_INSTALLED)
	papplClientHTMLPuts(client,
			    "        <p>To install or update the proprietary plugin please use the \"<font face=\"Courier\">hp-plugin</font>\" utility of HPLIP.</p>\n");
    }
    else
    {
      uri = papplClientGetURI(client);

      papplClientHTMLStartForm(client, uri, false);
      papplClientHTMLPuts(client,
			  "          <table class=\"form\">\n"
			  "            <tbody>\n");

      papplClientHTMLPrintf(client, "          <tr><td><button type=\"submit\" name=\"action\" value=\"install-plugin\">%s</button>",
			    plugin_status == HPLIP_PLUGIN_NOT_INSTALLED ?
			    "Install Plugin" :
			    (plugin_status == HPLIP_PLUGIN_INSTALLED ?
			     "Re-install Plugin" :
			     (plugin_status == HPLIP_PLUGIN_OUTDATED ?
			      "Update Plugin" :
			      "Install/Update Plugin")));
#if defined(SNAP) || HPLIP_OCI
      if (plugin_status != HPLIP_PLUGIN_NOT_INSTALLED)
	papplClientHTMLPuts(client, "&nbsp;<button type=\"submit\" name=\"action\" value=\"remove-plugin\">Remove Plugin</button>");
#endif // SNAP
      papplClientHTMLPuts(client,
			  "</td></tr>\n"
			  "            </tbody>\n"
			  "          </table>\n"
			  "        </form>\n");
    }

    if (plugin_status != HPLIP_PLUGIN_NOT_INSTALLED && licensetext)
    {
      papplClientHTMLPuts(client,
			  "        <h3>License</h3>\n");
      papplClientHTMLPuts(client,
			  "        <p>The proprietary plugin of HPLIP is released under the following user license:</p>\n");
      papplClientHTMLPrintf(client,
			    "        <div class=\"log\"><pre>%s</pre></div>\n",
			    licensetext);
    }
  }

  papplClientHTMLPuts(client,
                      "      </div>\n"
                      "    </div>\n");
  papplClientHTMLFooter(client);

 clean_up:
  // Clean up
  if (plugin_locked)
    pthread_mutex_unlock(&plugin_install_mutex);
  if (plugin_dir)
    free(plugin_dir);
  if (licensetext)
    free(licensetext);
}


//
// 'hplip_plugin_support()' - Callback function for the system
//                            setup. It updates an already installed
//                            plugin, if HPLIP got update to a new
//                            upstream version, it adds a button to
//                            the system part of the main page of the
//                            web interface, to open the page to
//                            initially install the plugin, and it
//                            adds this page to the web interface.
//

void
hplip_plugin_support(void *data)
{
  pr_printer_app_global_data_t *global_data =
    (pr_printer_app_global_data_t *)data;
  pappl_system_t   *system = prGetSystem(global_data);
  hplip_plugin_status_t plugin_status;
  char             *plugin_dir;
  char             errmsg[512] = "";


#if defined(SNAP) || HPLIP_OCI
  // Finish or undo an installation or removal which an abrupt restart
  // interrupted during the last run, before the plugin status gets read,
  // so that everything below, including the automatic update of an
  // outdated plugin, works on the plugin which is really installed
  hplip_recover_plugin(system);
#endif // SNAP || HPLIP_OCI

  // Get status of installed plugin
  plugin_status = hplip_plugin_status(system);

  if (plugin_status == HPLIP_PLUGIN_NOT_INSTALLED)
    papplLog(system, PAPPL_LOGLEVEL_DEBUG,
	     "Proprietary plugin is not installed.");
  else if (plugin_status == HPLIP_PLUGIN_INSTALLED)
    papplLog(system, PAPPL_LOGLEVEL_DEBUG,
	     "Proprietary plugin is installed and up-to-date.");
  else if (plugin_status == HPLIP_PLUGIN_OUTDATED)
  {
    // We need to update the plugin

    // Snap needs root; the OCI appliance modifies only its user-owned volume.
    if (HPLIP_CAN_INSTALL_PLUGIN)
    {
      // Serialize against web-admin install/update requests (see
      // hplip_web_plugin). The startup auto-update blocks until the lock
      // is free.
      pthread_mutex_lock(&plugin_install_mutex);
      papplLog(system, PAPPL_LOGLEVEL_DEBUG,
	       "Updating an already installed proprietary plugin ...");
      if ((plugin_dir = hplip_download_plugin(system, errmsg,
					      sizeof(errmsg))) == NULL)
      {
	papplLog(system, PAPPL_LOGLEVEL_ERROR,
		 "Unable to download plugin: %s", errmsg);
      }
      else
      {
	papplLog(system, PAPPL_LOGLEVEL_DEBUG,
		 "Plugin downloaded to %s", plugin_dir);

	if (hplip_install_plugin(system, plugin_dir))
	  papplLog(system, PAPPL_LOGLEVEL_DEBUG,
		   "Plugin installed.");
	else
	  papplLog(system, PAPPL_LOGLEVEL_ERROR,
		   "Plugin installation failed.");

	free(plugin_dir);
      }
      pthread_mutex_unlock(&plugin_install_mutex);
    }
  }

  // Add web interface page to manage the plugin
  papplSystemAddResourceCallback(system, "/plugin", "text/html",
				 (pappl_resource_cb_t)hplip_web_plugin,
				 global_data);
  papplSystemAddLink(system,
		     HPLIP_CAN_INSTALL_PLUGIN ? "Install Proprietary Plugin" :
		     "Proprietary Plugin Status",
		     "/plugin",
		     PAPPL_LOPTIONS_OTHER | PAPPL_LOPTIONS_HTTPS_REQUIRED);
}


//
// 'hplip_printer_extra_web_if()' - Add button for plugin web
//                                  interface page to print queue
//                                  entries which need the plugin.
//                                  Also call original function for
//                                  adding the "Device Settings",
//                                  page, needed especially for the
//                                  PostScript printers.
//

void
hplip_printer_extra_web_if(pappl_printer_t *printer, // I - Printer
			   void *data)               // I - Global data
{
  pr_printer_app_global_data_t *global_data =
    (pr_printer_app_global_data_t *)data;
  pappl_system_t   *system = prGetSystem(global_data);
  pappl_pr_driver_data_t driver_data;


  // "Device Settings" page for PPDs with "Installable Options" group or
  // PostScript query code for printer settings
  prSetupDeviceSettingsPage(printer, data);

  // pappl-retrofit registers the "Device Settings" admin form without
  // checking the server options, so remove it when no-web-interface is set
  if (!(papplSystemGetOptions(system) & PAPPL_SOPTIONS_WEB_INTERFACE))
  {
    char path[256];                         // Device settings page path

    papplPrinterGetPath(printer, "device", path, sizeof(path));
    papplSystemRemoveResource(system, path);
    papplPrinterRemoveLink(printer, "Device Settings");
  }

  papplPrinterGetDriverData(printer, &driver_data);
  if (strcasestr(driver_data.make_and_model, "proprietary plugin"))
  {
    // Printer needs the plugin, add a "Plugin" button to get to the
    // plugin management web interface page to the printer's entry
    papplPrinterAddLink(printer, "Plugin", "/plugin",
			PAPPL_LOPTIONS_NAVIGATION | PAPPL_LOPTIONS_STATUS);
  }
}


//
// 'main()' - Main entry for the hplip-printer-app.
//

int
main(int  argc,				// I - Number of command-line arguments
     char *argv[])			// I - Command-line arguments
{
  cups_array_t *spooling_conversions,
               *stream_formats,
               *driver_selection_regex_list;

  // Array of spooling conversions, most desirables first
  //
  // Here we prefer not converting into another format
  // Keeping vector formats (like PS -> PDF) is usually more desirable
  // but as many printers have buggy PS interpreters we prefer converting
  // PDF to Raster and not to PS
  spooling_conversions = cupsArrayNew(NULL, NULL);
  cupsArrayAdd(spooling_conversions, (void *)&PR_CONVERT_PDF_TO_PS);
  cupsArrayAdd(spooling_conversions, (void *)&PR_CONVERT_PDF_TO_RASTER);
  cupsArrayAdd(spooling_conversions, (void *)&PR_CONVERT_PS_TO_PS);
  cupsArrayAdd(spooling_conversions, (void *)&PR_CONVERT_PS_TO_RASTER);

  // Array of stream formats, most desirables first
  //
  // PDF comes last because it is generally not streamable.
  // PostScript comes second as it is Ghostscript's streamable
  // input format.
  stream_formats = cupsArrayNew(NULL, NULL);
  cupsArrayAdd(stream_formats, (void *)&PR_STREAM_CUPS_RASTER);
  cupsArrayAdd(stream_formats, (void *)&PR_STREAM_POSTSCRIPT);

  // Configuration record of the Printer Application
  pr_printer_app_config_t printer_app_config =
  {
    SYSTEM_NAME,              // Display name for Printer Application
    SYSTEM_PACKAGE_NAME,      // Package/executable name
    SYSTEM_VERSION_STR,       // Version as a string
    {
      SYSTEM_VERSION_ARR_0,   // Version 1st number
      SYSTEM_VERSION_ARR_1,   //         2nd
      SYSTEM_VERSION_ARR_2,   //         3rd
      SYSTEM_VERSION_ARR_3    //         4th
    },
    SYSTEM_WEB_IF_FOOTER,     // Footer for web interface (in HTML)
    // pappl-retrofit special features to be used
    PR_COPTIONS_QUERY_PS_DEFAULTS |
    PR_COPTIONS_NO_GENERIC_DRIVER |
#if !SNAP
    PR_COPTIONS_USE_ONLY_MATCHING_NICKNAMES |
#endif // !SNAP
    PR_COPTIONS_NO_PAPPL_BACKENDS |
    PR_COPTIONS_CUPS_BACKENDS,
    prAutoAdd,               // Auto-add (driver assignment) callback
    NULL,                     // Printer identify callback (HPLIP backend
                              // does not support this)
    prTestPage,              // Test page print callback
    hplip_plugin_support,     // Update installed plugin during system setup
                              // and add web interface button and page for
                              // plugin download
    hplip_printer_extra_web_if, // Set up "Device Settings" printer web
                              // interface page and also add a link to
                              // the plugin web interface page to entries
                              // of printers which need the plugin.
    spooling_conversions,     // Array of data format conversion rules for
                              // printing in spooling mode
    stream_formats,           // Arrray for stream formats to be generated
                              // when printing in streaming mode
    "",                       // CUPS backends to be ignored
    "hp,HP,snmp,dnssd,usb",   // CUPS backends to be used exclusively
                              // If empty all but the ignored backends are used
    TESTPAGE,                 // Test page (printable file), used by the
                              // standard test print callback prTestPage()
    ", +hpcups +[0-9]+\\.[0-9]+\\.[0-9]+[, ]*(.*)$|(\\W*[Pp]ost[Ss]cript).*$",
                              // Regular expression to separate the
                              // extra information after make/model in
                              // the PPD's *NickName. Also extracts a
                              // contained driver name (by using
                              // parentheses)
    NULL
                              // Regular expression for the driver
                              // auto-selection to prioritize a driver
                              // when there is more than one for a
                              // given printer. If a regular
                              // expression matches on the driver
                              // name, the driver gets priority. If
                              // there is more than one matching
                              // driver, the driver name on which the
                              // earlier regular expression in the
                              // list matches, gets the priority.
  };

  return (prRetroFitPrinterApp(&printer_app_config, argc, argv));
}
