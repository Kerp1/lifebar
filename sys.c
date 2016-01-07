#include "lifebar.h"

#include <alsa/asoundlib.h>
#include <math.h>
#include <regex.h>

#define PS_PATH "/sys/class/power_supply"
#define TH_PATH "/sys/class/thermal"
#define NET_PATH "/sys/class/net"

int count_acpi_batteries() {
   DIR *d;
   struct dirent *de;
   int count = 0;

   d = opendir(PS_PATH);
   if(d == NULL) return 0;
   while((de = readdir(d)) != NULL) {
      if(strstr(de->d_name, "BAT") == de->d_name) {
         //name starts with BAT
         count++;
      }
   }
   closedir(d);

   return count;
}

int count_acpi_thermal() {
   DIR *d;
   struct dirent *de;
   int count = 0;

   d = opendir(TH_PATH);
   if(d == NULL) return 0;
   while((de = readdir(d)) != NULL) {
      if(strstr(de->d_name, "thermal") == de->d_name) {
         //name starts with thermal
         count++;
      }
   }
   closedir(d);

   return count;
}

void get_match_string(char *buffer, regmatch_t* match, char *str) {
  size_t size = match->rm_eo - match->rm_so;
  strncpy(buffer, str + match->rm_so, size);
  buffer[size] = '\0';
}

int get_battery_state(char* state) {
  if(strstr(state, "Charging") != NULL) {
    return CHARGING;
  } else if(strstr(state, "Discharging") != NULL) {
    return DISCHARGING;
  } else {
    return UNKNOWN;
  }
}

void read_acpi_battery(int b, struct batt_info *bi) {
  char cmd[] = "acpi";
  char command_output[512];
  char buffer[128];
  FILE *file;
  regex_t regex;
  regcomp(&regex, "Battery ([0-9]): (.+?), ([0-9]+)., (.{2}:.{2}:.{2}) ", REG_EXTENDED);

  size_t ngroups = regex.re_nsub + 1;
  regmatch_t *groups = malloc(ngroups * sizeof(regmatch_t));

  file = popen(cmd, "r");
  if(file == NULL) {
    fprintf(stderr, "%sCouldn't execute acpi command\n", BAD_MSG, NULL);

    bi->index = 0;
    bi->percent = 20;
    bi->status = UNKNOWN;

    return;
  }
  
  while(fgets(command_output, sizeof(command_output) - 1, file) != NULL) {
    int ret = regexec(&regex, command_output, ngroups, groups, 0);

    if(ret != 0) {
      bi->index = 0;
      bi->percent = 0;
      bi->status = UNKNOWN;
      return;
    }
    //Calculate the number of match groups.
    size_t nmatched;
    for(nmatched = 0; nmatched < ngroups; ++nmatched) {
      if(groups[nmatched].rm_so == -1) {
        break;
      }
    }

    if(nmatched != ngroups) {
      fprintf(stderr, "%sCould not match string: '%s'\n",
      BAD_MSG, command_output);
    }

    get_match_string(buffer, &groups[1], command_output);
    bi->index = strtol(buffer, NULL, 10);

    get_match_string(buffer, &groups[2], command_output);
    bi->status = get_battery_state(buffer);

    get_match_string(buffer, &groups[3], command_output);
    bi->percent = strtol(buffer, NULL, 10);

    get_match_string(buffer, &groups[4], command_output);
    strcpy(bi->time_left, buffer);

    regfree(&regex);
  }
  pclose(file);
}

void read_acpi_thermal(int t, struct thermal_info *therm) {
   //save the index 
   therm->index = t;

   //we assume the thermal index to exist as filename thermal_zone<index>
   char path[128];
   FILE *f;

   //temp
   char temp_s[32];
   long int temp = 0;
   sprintf(path, "%s/thermal_zone%d/temp", TH_PATH, t);
   f = fopen(path, "r");
   if(f == NULL || fgets(temp_s, 32, f) == NULL) {
      fprintf(stderr, "%scould not read thermal status: '%s'\n",
            BAD_MSG, path);
   }
   else {
        temp = strtol(temp_s, NULL, 10);
        fclose(f);
    }

   therm->temp_c = temp / 1000;
}

void read_net_speed(char *ifname, struct net_speed_info *net) {
   char path[128];
   FILE *f;

   //download
   char rxb_s[32];
   sprintf(path, "%s/%s/statistics/rx_bytes", NET_PATH, ifname);
   f = fopen(path, "r");
   if(f == NULL || fgets(rxb_s, 32, f) == NULL) {
      fprintf(stderr, "%scould not read interface speed: '%s'\n",
            BAD_MSG, path);
   }
   else {
        net->down_bytes = strtol(rxb_s, NULL, 10);
        fclose(f);
    }

   //upload
   char txb_s[32];
   sprintf(path, "%s/%s/statistics/tx_bytes", NET_PATH, ifname);
   f = fopen(path, "r");
   if(f == NULL || fgets(txb_s, 32, f) == NULL) {
      fprintf(stderr, "%scould not read interface speed: '%s'\n",
            BAD_MSG, path);
   }
   else {
        net->up_bytes = strtol(txb_s, NULL, 10);
        fclose(f);
    }
}

void read_net_info(char *ifname, struct net_info *net) {
   FILE *file;
   char path[512];
   char first[] = "/usr/bin/iw dev ";
   char last[] = " link\n";
   char cmd[64];

   //Create command to run, has the structure [first] + ifname + [last] + \0"
   strncpy(cmd, first, strlen(first));
   strncpy(cmd + strlen(first), ifname, strlen(ifname));
   strncpy(cmd + strlen(first) + strlen(ifname), last, strlen(last));
   strncpy(cmd + strlen(first) + strlen(ifname) + strlen(last), "\0", 1);

   file = popen(cmd, "r");
   if(file == NULL) {
      fprintf(stderr, "%sCouldn't execute iw command\n",
         BAD_MSG, NULL);

      strncpy(net->name, "Error", 6);
      strncpy(net->signal_level, "0", 2);

      return;
   }

   while(fgets(path, sizeof(path) - 1, file) != NULL) {
      if(strstr(path, "SSID: ") != NULL) {
         int ssid_length = strlen(path) - 7;

         strncpy(net->name, path + 7, ssid_length);
         strncpy(net->name + ssid_length - 1, "\0", 1);

      } else if(strstr(path, "signal:") != NULL) {
         int signal_length = 3;

         strncpy(net->signal_level, path + 9, signal_length);
         strncpy(net->signal_level + signal_length, "\0", 1);

      } else if(strstr(path, "Not connected") != NULL) {
         strncpy(net->name, "Down", 5);
         strncpy(net->signal_level, "0", 2);
      } 
   }
   pclose(file);
}

int is_muted(snd_mixer_elem_t *elem) {
   int value = 0;

   if (snd_mixer_selem_has_playback_switch(elem)) {
        snd_mixer_selem_get_playback_switch(elem, SND_MIXER_SCHN_MONO, &value);
    }
   //The value returned indicates whether the switch is not muted, so we have to reverse it.
   value = value ? 0 : 1;
   return value;
}

void get_alsa_master_info(struct volume_info *info)
{
    long min, max;
    long volume;
    snd_mixer_t *handle;
    snd_mixer_selem_id_t *sid;
    const char *card = "default";
    const char *selem_name = "Master";

    snd_mixer_open(&handle, 0);
    snd_mixer_attach(handle, card);
    snd_mixer_selem_register(handle, NULL, NULL);
    snd_mixer_load(handle);

    snd_mixer_selem_id_alloca(&sid);
    snd_mixer_selem_id_set_index(sid, 0);
    snd_mixer_selem_id_set_name(sid, selem_name);
    snd_mixer_elem_t* elem = snd_mixer_find_selem(handle, sid);

    if(elem == NULL) {
      info->volume_percent = 999;
      info->is_muted = 0;
      snd_mixer_close(handle);
      return;
    }

    snd_mixer_selem_get_playback_volume_range(elem, &min, &max);
    snd_mixer_selem_get_playback_volume (elem, SND_MIXER_SCHN_MONO, &volume);

    float percent = (volume / (float)max) * 100;


    info->volume_percent = (int)percent;
    info->is_muted = is_muted(elem);

    snd_mixer_close(handle);
}
