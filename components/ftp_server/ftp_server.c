//-- SPDX-License-Identifier: GPL-3.0-or-later
//-- Copyright (C) 2026 Willem Aandewiel

#include "ftp_server.h"
#include "ftp_protocol.h"
#include <arpa/inet.h>
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#define FTP_PATH_MAX 512
#define FTP_LINE_MAX 512
#define FTP_TAG "ftp_server"

typedef struct { char base[FTP_PATH_MAX]; uint16_t port, pmin, pmax, next_port; size_t bufsize, max_clients; uint32_t ctimeout, dtimeout; } server_cfg_t;
typedef struct session_t { int ctrl, pasv; char cwd[FTP_PATH_MAX], rename_from[FTP_PATH_MAX]; off_t rest; struct session_t *next; } session_t;
static server_cfg_t g;
static volatile bool running;
static int listen_fd=-1;
static TaskHandle_t server_task;
static SemaphoreHandle_t lock;
static size_t clients;
static session_t *sessions;
static TaskHandle_t restart_task;

extern esp_err_t esp_littlefs_info(const char *partition_label,
								   size_t *total_bytes,
								   size_t *used_bytes) __attribute__((weak));
extern esp_err_t esp_vfs_fat_info(const char *base_path,
								  uint64_t *total_bytes,
								  uint64_t *free_bytes) __attribute__((weak));

static void format_size(uint64_t bytes, char *text, size_t capacity)
{
	const char *unit = "Bytes";
	uint64_t divisor = 1;
	if (bytes > 1000000)
	{
		unit = "MB";
		divisor = 1000000;
	}
	else if (bytes > 1000)
	{
		unit = "KB";
		divisor = 1000;
	}
	snprintf(text, capacity, "%llu %s", bytes / divisor, unit);
}

static void storage_report(const char *operation, const char *path, int result)
{
	struct statvfs stats;
	uint64_t total = 0;
	uint64_t used = 0;
	uint64_t free = 0;
	bool available = statvfs(g.base, &stats) == 0;
	if (available)
	{
		total = (uint64_t)stats.f_blocks * stats.f_frsize;
		free = (uint64_t)stats.f_bfree * stats.f_frsize;
		used = total >= free ? total - free : 0;
	}
	else
	{
		size_t littlefs_total = 0;
		size_t littlefs_used = 0;
		if (esp_littlefs_info &&
			esp_littlefs_info("storage", &littlefs_total, &littlefs_used) == ESP_OK)
		{
			total = littlefs_total;
			used = littlefs_used;
			free = total >= used ? total - used : 0;
			available = true;
		}
	}
	if (!available && esp_vfs_fat_info && esp_vfs_fat_info(g.base, &total, &free) == ESP_OK)
	{
		used = total >= free ? total - free : 0;
		available = true;
	}

	if (!available)
	{
		ESP_LOGE(FTP_TAG, "%s %s failed: storage statistics unavailable (%s)",
				 operation, path, strerror(errno));
		return;
	}
	char total_text[24], used_text[24], free_text[24];
	format_size(total, total_text, sizeof(total_text));
	format_size(used, used_text, sizeof(used_text));
	format_size(free, free_text, sizeof(free_text));

	if (result == 0)
	{
		ESP_LOGI(FTP_TAG, "%s %s succeeded; storage: total=%s, used=%s, free=%s",
				 operation, path, total_text, used_text, free_text);
	}
	else
	{
		ESP_LOGW(FTP_TAG, "%s %s failed; storage: total=%s, used=%s, free=%s",
				 operation, path, total_text, used_text, free_text);
	}
}

static void closefd(int *fd){ if(*fd>=0){ shutdown(*fd,SHUT_RDWR); close(*fd); *fd=-1; } }
static int send_all(int fd,const void *data,size_t n){ const char *p=data; while(n){ ssize_t k=send(fd,p,n,0); if(k<0&&errno==EINTR)continue; if(k<=0)return -1; p+=k;n-=k;vTaskDelay(1);}return 0; }
static int reply(session_t*s,int code,const char*fmt,...){ char line[420];va_list ap;va_start(ap,fmt);int n=ftp_format_response_v(code,line,sizeof(line),fmt,ap);va_end(ap);return n>0?send_all(s->ctrl,line,n):-1;}
static bool normalize(const char *cwd,const char *arg,char*out,size_t cap){return ftp_normalize_path(cwd,arg,out,cap);}
static bool paths(session_t*s,const char*arg,char*virt,char*vfs){if(!normalize(s->cwd,arg,virt,FTP_PATH_MAX))return false;int n=snprintf(vfs,FTP_PATH_MAX,"%s%s",g.base,!strcmp(virt,"/")?"":virt);return n>0&&n<FTP_PATH_MAX;}
static void timeout_fd(int fd,uint32_t ms){struct timeval tv={.tv_sec=ms/1000,.tv_usec=(ms%1000)*1000};setsockopt(fd,SOL_SOCKET,SO_RCVTIMEO,&tv,sizeof(tv));setsockopt(fd,SOL_SOCKET,SO_SNDTIMEO,&tv,sizeof(tv));}
static int open_data(session_t*s,bool upload){if(s->pasv<0)return -1;int fd=accept(s->pasv,NULL,NULL);closefd(&s->pasv);if(fd>=0){struct timeval tv={.tv_sec=g.dtimeout/1000,.tv_usec=(g.dtimeout%1000)*1000};setsockopt(fd,SOL_SOCKET,upload?SO_RCVTIMEO:SO_SNDTIMEO,&tv,sizeof(tv));}return fd;}
static void pasv(session_t*s,bool epsv){closefd(&s->pasv);for(unsigned i=0;i<=g.pmax-g.pmin;i++){uint16_t p; xSemaphoreTake(lock,portMAX_DELAY);p=g.next_port++;if(g.next_port>g.pmax)g.next_port=g.pmin;xSemaphoreGive(lock);int fd=socket(AF_INET,SOCK_STREAM,IPPROTO_IP);if(fd<0)continue;int one=1;setsockopt(fd,SOL_SOCKET,SO_REUSEADDR,&one,sizeof(one));struct sockaddr_in a={.sin_family=AF_INET,.sin_port=htons(p),.sin_addr.s_addr=INADDR_ANY};if(bind(fd,(void*)&a,sizeof(a))==0&&listen(fd,1)==0){s->pasv=fd;if(epsv){reply(s,229,"Entering Extended Passive Mode (|||%u|).",p);}else{struct sockaddr_in local;socklen_t n=sizeof(local);getsockname(s->ctrl,(void*)&local,&n);uint32_t ip=ntohl(local.sin_addr.s_addr);reply(s,227,"Entering Passive Mode (%u,%u,%u,%u,%u,%u).",(ip>>24)&255,(ip>>16)&255,(ip>>8)&255,ip&255,p>>8,p&255);}return;}close(fd);}reply(s,425,"Cannot open passive listener.");}
static int listing(session_t*s,const char*arg,bool names){char v[FTP_PATH_MAX],p[FTP_PATH_MAX];if(!paths(s,arg&&*arg?arg:NULL,v,p))return reply(s,550,"Invalid path.");DIR*d=opendir(p);if(!d)return reply(s,550,"Directory unavailable.");reply(s,150,"Opening data connection.");int data=open_data(s,false);if(data<0){closedir(d);return reply(s,425,"Cannot open data connection.");}struct dirent*e;char line[768],full[FTP_PATH_MAX+256+2];while((e=readdir(d))){if(!strcmp(e->d_name,".")||!strcmp(e->d_name,".."))continue;if(names)snprintf(line,sizeof(line),"%s\r\n",e->d_name);else{snprintf(full,sizeof(full),"%s/%s",p,e->d_name);struct stat st={0};stat(full,&st);struct tm tm={0};time_t when=st.st_mtime?st.st_mtime:time(NULL);localtime_r(&when,&tm);char date[32];strftime(date,sizeof(date),"%b %d %H:%M",&tm);snprintf(line,sizeof(line),"%crw-r--r-- 1 ftp ftp %10lld %s %s\r\n",S_ISDIR(st.st_mode)?'d':'-',(long long)(S_ISDIR(st.st_mode)?0:st.st_size),date,e->d_name);}if(send_all(data,line,strlen(line)))break;}closedir(d);closefd(&data);return reply(s,226,"Transfer complete.");}
static int transfer(session_t*s,const char*arg,bool upload)
{
	char v[FTP_PATH_MAX], p[FTP_PATH_MAX];
	if (!paths(s, arg, v, p))
	{
		return reply(s, 550, "Invalid path.");
	}
	int f = open(p, upload ? (O_WRONLY | O_CREAT | (s->rest ? 0 : O_TRUNC)) : O_RDONLY, 0644);
	if (f < 0)
	{
		storage_report(upload ? "STOR" : "RETR", p, -1);
		return reply(s, 550, "File unavailable.");
	}
	if (s->rest && lseek(f, s->rest, SEEK_SET) < 0)
	{
		close(f);
		s->rest = 0;
		storage_report(upload ? "STOR" : "RETR", p, -1);
		return reply(s, 550, "Invalid restart position.");
	}
	s->rest = 0;
	ESP_LOGI(FTP_TAG, "%s %s waiting for data connection", upload ? "STOR" : "RETR", p);
	reply(s, 150, "Opening binary data connection.");
	int data = open_data(s, upload);
	if (data < 0)
	{
		close(f);
		storage_report(upload ? "STOR" : "RETR", p, -1);
		return reply(s, 425, "Cannot open data connection.");
	}
	ESP_LOGI(FTP_TAG, "%s %s data connection opened", upload ? "STOR" : "RETR", p);
	char *b = malloc(g.bufsize);
	int transfer_errno = 0;
	size_t transferred = 0;
	bool ok = b;
	while (ok)
	{
		ssize_t n = upload ? recv(data, b, g.bufsize, 0) : read(f, b, g.bufsize);
		if (n == 0)
		{
			break;
		}
		if (n < 0)
		{
			if (errno == EINTR)
			{
				continue;
			}
			transfer_errno = errno;
			ESP_LOGW(FTP_TAG, "%s %s data read failed after %u bytes: %s",
							 upload ? "STOR" : "RETR", p, (unsigned)transferred, strerror(errno));
			ok = false;
			break;
		}
		if (upload)
		{
			char *q = b;
			ssize_t left = n;
			while (left)
			{
				ssize_t k = write(f, q, left);
				if (k < 0 && errno == EINTR)
				{
					continue;
				}
				if (k <= 0)
				{
					transfer_errno = errno;
					ESP_LOGW(FTP_TAG, "STOR %s write failed after %u bytes: %s",
									 p, (unsigned)transferred, strerror(errno));
					ok = false;
					break;
				}
				q += k;
				left -= k;
				transferred += (size_t)k;
			}
		}
			else if (send_all(data, b, n))
		{
			transfer_errno = errno;
				ESP_LOGW(FTP_TAG, "RETR %s data send failed after %u bytes: %s",
								 p, (unsigned)transferred, strerror(errno));
			ok = false;
			break;
		}
			else
			{
				transferred += (size_t)n;
			}
	}
	free(b);
	ESP_LOGI(FTP_TAG, "%s %s data stream ended after %u bytes; syncing",
					 upload ? "STOR" : "RETR", p, (unsigned)transferred);
	if (upload && fsync(f) < 0)
	{
		transfer_errno = errno;
		ESP_LOGW(FTP_TAG, "STOR %s fsync failed: %s", p, strerror(errno));
		ok = false;
	}
	close(f);
	if (ok && !upload)
	{
		shutdown(data, SHUT_WR);
	}
	closefd(&data);
	storage_report(upload ? "STOR" : "RETR", p, ok ? 0 : -1);
	return reply(s, !ok && transfer_errno == ENOSPC ? 552 : (ok ? 226 : 426),
							 !ok && transfer_errno == ENOSPC ? "Storage allocation failed." :
							 (ok ? "Transfer complete." : "Transfer aborted."));
}
#if CONFIG_FTP_SERVER_ENABLE_TEST_HOOK
static void restart_task_fn(void *arg)
{
	(void)arg;
	char base_path[FTP_PATH_MAX];
	snprintf(base_path, sizeof(base_path), "%s", g.base);
	ftp_server_config_t config = FTP_SERVER_DEFAULT_CONFIG();
	config.base_path = base_path;
	config.control_port = g.port;
	config.passive_port_min = g.pmin;
	config.passive_port_max = g.pmax;
	config.transfer_buffer_size = g.bufsize;
	config.control_timeout_ms = g.ctimeout;
	config.data_timeout_ms = g.dtimeout;
	config.max_clients = g.max_clients;
	if (ftp_server_stop() != ESP_OK)
	{
		restart_task = NULL;
		vTaskDelete(NULL);
		return;
	}
	restart_task = NULL;
	ftp_server_start(&config);
	vTaskDelete(NULL);
}
#endif
static bool command(session_t*s,char*line){char*arg=strchr(line,' ');if(arg){*arg++=0;while(*arg==' ')arg++;}else arg="";for(char*p=line;*p;p++)*p=toupper((unsigned char)*p);if((!strcmp(line,"CWD")||!strcmp(line,"RETR")||!strcmp(line,"STOR")||!strcmp(line,"SIZE")||!strcmp(line,"MDTM")||!strcmp(line,"MKD")||!strcmp(line,"XMKD")||!strcmp(line,"RMD")||!strcmp(line,"XRMD")||!strcmp(line,"DELE")||!strcmp(line,"RNFR")||!strcmp(line,"RNTO"))&&!arg[0]){reply(s,501,"Argument required.");return true;}char v[FTP_PATH_MAX],p[FTP_PATH_MAX];p[0]=0;struct stat st;
 if(!strcmp(line,"USER")||!strcmp(line,"PASS"))reply(s,230,"Login not required.");
 else if(!strcmp(line,"SYST"))reply(s,215,"UNIX Type: L8");
 else if(!strcmp(line,"FEAT")){const char*f="211-Features\r\n UTF8\r\n EPSV\r\n MDTM\r\n SIZE\r\n REST STREAM\r\n211 End\r\n";send_all(s->ctrl,f,strlen(f));}
 else if(!strcmp(line,"PWD")||!strcmp(line,"XPWD"))reply(s,257,"\"%s\" is current directory.",s->cwd);
 else if(!strcmp(line,"CWD")){if(paths(s,arg,v,p)&&!stat(p,&st)&&S_ISDIR(st.st_mode)){strcpy(s->cwd,v);reply(s,250,"Directory changed.");}else reply(s,550,"Directory unavailable.");}
 else if(!strcmp(line,"CDUP")){if(paths(s,"..",v,p)){strcpy(s->cwd,v);reply(s,250,"Directory changed.");}}
 else if(!strcmp(line,"TYPE")){if(!strcmp(arg,"I")||!strcmp(arg,"A"))reply(s,200,"Type set.");else reply(s,501,"Unsupported transfer type.");}
 else if(!strcmp(line,"PASV"))pasv(s,false); else if(!strcmp(line,"EPSV"))pasv(s,true);
 else if(!strcmp(line,"LIST"))listing(s,arg,false); else if(!strcmp(line,"NLST"))listing(s,arg,true);
 else if(!strcmp(line,"RETR"))transfer(s,arg,false); else if(!strcmp(line,"STOR"))transfer(s,arg,true);
 else if(!strcmp(line,"SIZE")){if(paths(s,arg,v,p)&&!stat(p,&st)&&S_ISREG(st.st_mode))reply(s,213,"%lld",(long long)st.st_size);else reply(s,550,"File unavailable.");}
 else if(!strcmp(line,"MDTM")){if(paths(s,arg,v,p)&&!stat(p,&st)){struct tm tm;gmtime_r(&st.st_mtime,&tm);char t[20];strftime(t,sizeof(t),"%Y%m%d%H%M%S",&tm);reply(s,213,"%s",t);}else reply(s,550,"File unavailable.");}
 else if(!strcmp(line,"REST")){char*end;s->rest=strtoll(arg,&end,10);if(!arg[0]||*end||s->rest<0){s->rest=0;reply(s,501,"Invalid offset.");}else reply(s,350,"Restart position accepted.");}
 else if(!strcmp(line,"MKD")||!strcmp(line,"XMKD")){bool ok=paths(s,arg,v,p)&&mkdir(p,0755)==0;storage_report("MKD",p,ok?0:-1);reply(s,ok?257:550,ok?"\"%s\" created.":"Create directory failed.",v);}
 else if(!strcmp(line,"RMD")||!strcmp(line,"XRMD")){bool ok=paths(s,arg,v,p)&&rmdir(p)==0;storage_report("RMD",p,ok?0:-1);reply(s,ok?250:550,ok?"Directory removed.":"Remove directory failed.");}
 else if(!strcmp(line,"DELE")){bool ok=paths(s,arg,v,p)&&unlink(p)==0;storage_report("DELE",p,ok?0:-1);reply(s,ok?250:550,ok?"File deleted.":"Delete failed.");}
 else if(!strcmp(line,"RNFR")){if(paths(s,arg,v,p)&&!stat(p,&st)){strcpy(s->rename_from,p);storage_report("RNFR",p,0);reply(s,350,"Ready for destination name.");}else{storage_report("RNFR",p,-1);reply(s,550,"Source unavailable.");}}
 else if(!strcmp(line,"RNTO")){if(!s->rename_from[0])reply(s,503,"RNFR required first.");else{bool ok=paths(s,arg,v,p)&&rename(s->rename_from,p)==0;storage_report("RNTO",p,ok?0:-1);s->rename_from[0]=0;reply(s,ok?250:550,ok?"Rename successful.":"Rename failed.");}}
 else if(!strcmp(line,"OPTS")&&!strcasecmp(arg,"UTF8 ON"))reply(s,200,"UTF8 enabled.");
 else if(!strcmp(line,"OPTS"))reply(s,501,"Unsupported option.");
 else if(!strcmp(line,"ABOR")){closefd(&s->pasv);reply(s,226,"Abort successful.");}
#if CONFIG_FTP_SERVER_ENABLE_TEST_HOOK
 else if(!strcasecmp(line,"XTEST")&&!strcasecmp(arg,"RESTART")){if(restart_task||xTaskCreate(restart_task_fn,"ftp_restart",4096,NULL,5,&restart_task)!=pdPASS)reply(s,451,"Restart unavailable.");else reply(s,200,"Restart scheduled.");}
#endif
 else if(!strcmp(line,"NOOP"))reply(s,200,"OK.");
 else if(!strcmp(line,"QUIT")){reply(s,221,"Goodbye.");return false;}
 else{ESP_LOGW(FTP_TAG,"Unsupported FTP command: %s",line);reply(s,502,"Command not implemented.");}
 return true;}
static void session_task(void*arg){session_t*s=arg;reply(s,220,"ESP32 FTP server ready.");char line[FTP_LINE_MAX];size_t used=0;bool stay=true;while(running&&stay){char c;ssize_t n=recv(s->ctrl,&c,1,0);if(n<=0)break;bool complete=false,overflow=false;ftp_parser_feed(line,&used,c,&complete,&overflow);if(overflow){reply(s,500,"Command line too long.");continue;}if(complete){if(used)stay=command(s,line);used=0;}}closefd(&s->pasv);closefd(&s->ctrl);xSemaphoreTake(lock,portMAX_DELAY);session_t **current=&sessions;while(*current&&*current!=s)current=&(*current)->next;if(*current==s)*current=s->next;clients--;xSemaphoreGive(lock);free(s);vTaskDelete(NULL);}
static void accept_task(void*arg){(void)arg;while(running){int fd=accept(listen_fd,NULL,NULL);if(fd<0){if(running)vTaskDelay(pdMS_TO_TICKS(50));continue;}xSemaphoreTake(lock,portMAX_DELAY);bool room=clients<g.max_clients;if(room)clients++;xSemaphoreGive(lock);if(!room){send_all(fd,"421 Too many clients.\r\n",23);close(fd);continue;}session_t*s=calloc(1,sizeof(*s));if(!s){close(fd);xSemaphoreTake(lock,portMAX_DELAY);clients--;xSemaphoreGive(lock);continue;}s->ctrl=fd;s->pasv=-1;strcpy(s->cwd,"/");timeout_fd(fd,g.ctimeout);xSemaphoreTake(lock,portMAX_DELAY);s->next=sessions;sessions=s;xSemaphoreGive(lock);if(xTaskCreate(session_task,"ftp_session",12288,s,5,NULL)!=pdPASS){xSemaphoreTake(lock,portMAX_DELAY);sessions=s->next;clients--;xSemaphoreGive(lock);close(fd);free(s);}}server_task=NULL;vTaskDelete(NULL);}
esp_err_t ftp_server_start(const ftp_server_config_t*c){if(running)return ESP_ERR_INVALID_STATE;if(!c||!c->base_path||c->passive_port_min>c->passive_port_max||!c->max_clients)return ESP_ERR_INVALID_ARG;struct stat st;if(stat(c->base_path,&st)||!S_ISDIR(st.st_mode))return ESP_ERR_NOT_FOUND;memset(&g,0,sizeof(g));sessions=NULL;clients=0;snprintf(g.base,sizeof(g.base),"%s",c->base_path);g.port=c->control_port;g.pmin=c->passive_port_min;g.pmax=c->passive_port_max;g.next_port=g.pmin;g.bufsize=c->transfer_buffer_size?c->transfer_buffer_size:4096;g.max_clients=c->max_clients;g.ctimeout=c->control_timeout_ms;g.dtimeout=c->data_timeout_ms;esp_log_level_set(FTP_TAG,(esp_log_level_t)CONFIG_FTP_SERVER_LOG_LEVEL_VALUE);lock=xSemaphoreCreateMutex();if(!lock)return ESP_ERR_NO_MEM;listen_fd=socket(AF_INET,SOCK_STREAM,IPPROTO_IP);if(listen_fd<0){vSemaphoreDelete(lock);lock=NULL;return ESP_FAIL;}int one=1;setsockopt(listen_fd,SOL_SOCKET,SO_REUSEADDR,&one,sizeof(one));struct sockaddr_in a={.sin_family=AF_INET,.sin_port=htons(g.port),.sin_addr.s_addr=INADDR_ANY};if(bind(listen_fd,(void*)&a,sizeof(a))||listen(listen_fd,g.max_clients)){closefd(&listen_fd);vSemaphoreDelete(lock);lock=NULL;return ESP_FAIL;}running=true;if(xTaskCreate(accept_task,"ftp_server",4096,NULL,5,&server_task)!=pdPASS){running=false;closefd(&listen_fd);vSemaphoreDelete(lock);lock=NULL;return ESP_ERR_NO_MEM;}ESP_LOGI(FTP_TAG,"Serving %s on port %u",g.base,g.port);storage_report("START",g.base,0);return ESP_OK;}
esp_err_t ftp_server_stop(void){if(!running)return ESP_ERR_INVALID_STATE;running=false;closefd(&listen_fd);xSemaphoreTake(lock,portMAX_DELAY);for(session_t *session=sessions;session;session=session->next){closefd(&session->pasv);shutdown(session->ctrl,SHUT_RDWR);}xSemaphoreGive(lock);for(int i=0;i<100&&server_task;i++)vTaskDelay(pdMS_TO_TICKS(10));for(int i=0;i<100&&clients;i++)vTaskDelay(pdMS_TO_TICKS(10));if(clients)return ESP_ERR_TIMEOUT;vSemaphoreDelete(lock);lock=NULL;return ESP_OK;}
bool ftp_server_is_running(void){return running;}
