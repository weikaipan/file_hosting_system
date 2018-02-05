#include "csiebox_server.h"

#include "csiebox_common.h"
#include "connect.h"

#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <assert.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <dirent.h>
#include <utime.h>
#include <signal.h>
#include <unistd.h>
#include <inttypes.h>

static int parse_arg(csiebox_server* server, int argc, char** argv);
// MP5 signal
char fifo_path[PATH_MAX];
void signal_handler_write(int num1);
void signal_handler_delete(int num2);
static void daemonize(csiebox_server* server);
// MP4  thread
static int task_fd[50] = {0};
struct thread_data* thread;
static int* working_status;
static void* handle_request(void *arg);
static void handle_busy(int conn_fd);
//static void handle_request(csiebox_server* server, int conn_fd);
// MP4
static int get_account_info(csiebox_server* server,  const char* user, csiebox_account_info* info);
static void login(csiebox_server* server, int conn_fd, csiebox_protocol_login* login);
static void logout(csiebox_server* server, int conn_fd);
static void sync_file(csiebox_server* server, int conn_fd, csiebox_protocol_meta* meta);
static char* get_user_homedir(csiebox_server* server, csiebox_client_info* info);
static void rm_file(csiebox_server* server, int conn_fd, csiebox_protocol_rm* rm);

void csiebox_server_init(csiebox_server** server, int argc, char** argv) {
  csiebox_server* tmp = (csiebox_server*)malloc(sizeof(csiebox_server));
  if (!tmp) {
    fprintf(stderr, "server malloc fail\n");
    return;
  }
  memset(tmp, 0, sizeof(csiebox_server));
  if (!parse_arg(tmp, argc, argv)) {
    fprintf(stderr, "Usage: %s [config file] [-d]\n", argv[0]);
    free(tmp);
    return;
  }

  int fd = server_start();
  if (fd < 0) {
    fprintf(stderr, "server fail\n");
    free(tmp);
    return;
  }
  tmp->client = (csiebox_client_info**)malloc(sizeof(csiebox_client_info*) * getdtablesize());
  if (!tmp->client) {
    fprintf(stderr, "client list malloc fail\n");
    close(fd);
    free(tmp);
    return;
  }
  memset(tmp->client, 0, sizeof(csiebox_client_info*) * getdtablesize());
  tmp->listen_fd = fd;
  *server = tmp;
}

void signal_handler_write(int num1) {
  fprintf(stderr, "catch signal user\n");
  fprintf(stderr, "%s\n", fifo_path);
  int fd = open(fifo_path, O_WRONLY);
  if (fd < 0) {
    fprintf(stderr, "Open fifo fail\n");
    return;
  }
  int working = 0, i;
  uint32_t t_num;
  char buf[4];
  for (i = 0; i < 16; i++)
    if (working_status[i] != -1)
      working++;
  fprintf(stderr, "current working %d.\n", working);
  t_num = htonl(working);
  fprintf(stderr, "%"PRIu32"\n", t_num);
  sprintf(buf, "%"PRIu32"", t_num);
  write(fd, buf, sizeof(buf));
}

void signal_handler_delete(int num2) {
  fprintf(stderr, "catch signal interrupt\n");
  if (unlink(fifo_path) < 0) {
    fprintf(stderr, "Delete fifo fail.\n");
    return;
  }
  exit(0);
  return;
}
/*
static void daemonize(csiebox_server* server) {
  pid_t process_id = 0;
  pid_t sid = 0;
  process_id = fork();
  if (process_id < 0) {
    fprintf(stderr, "Fork failed\n");
    exit(1);
  }
  if (process_id > 0) {
    exit(0);
  }
  umask(0);
  sid = setid();
  if (sid < 0) {
    exit(1);
  }
  if (chdir("/tmp") < 0 )
    fprintf(STDERR_FILENO, "Cannot change to tmp DIR\n");

  int fd0 = open("/dev/null", O_RDWR);
  int fd1 = dup2(fd0, STDIN_FILENO);
  int fd2 = dup2(fd0, STDOUT_FILENO);
  int fd3 = dup2(fd0, STDERR_FILENO);
  return;
}
*/
int csiebox_server_run(csiebox_server* server) { 
  // Do daemonize
  /*
  if (server->arg.daemonize) {
    daemonize(server);
  }
  */
  int conn_fd, conn_len, *tid;
  int i, j, rc, fdmax;
  long t;
  struct sockaddr_in addr;
  uint32_t t_num;
  char buf[128];
  t_num = htonl(12);
  fprintf(stderr, "t_num = %"PRIu32"\n", t_num);
  // MP4
  // prepare threads and thread_data
  pthread_t *worker;
  worker = (pthread_t*)malloc(sizeof(pthread_t)*(server->threads));
  thread = (struct thread_data*)malloc(sizeof(struct thread_data)*(server->threads));
  working_status = (int*)malloc(sizeof(int)*(server->threads));
  for (i = 0; i < server->threads; i++)
    working_status[i] = -1;
  fprintf(stderr, "%d threads required\n", server->threads);
  for (t = 0; t < server->threads; t++){
      thread[t].thread_id = t;
      thread[t].server = server;
      rc = pthread_create(&worker[t], NULL, 
		handle_request, (void *)&thread[t]);
      if (rc)
	fprintf(stderr, "Create thread %d error\n", rc);
      else
	fprintf(stderr, "Created thread %d.\n", t);
  }
  fprintf(stderr, "Finsih creating threads\n");
  //assert(working_status);
  // MP5
  // 1. GET PID
  pid_t pid;
  pid = getpid();
  fprintf(stderr, "Server pid = %d\n", (int)pid);
  sprintf(fifo_path, "%s/fifo.%d", server->arg.run, (int)pid);
  if (mkfifo(fifo_path, S_IWUSR | S_IRUSR |
			      S_IRGRP | S_IROTH) == 0)
    fprintf(stderr, "Create fifo %s\n", fifo_path);
  // 2. SIGNAL USR1
  struct sigaction saw, sad;
  saw.sa_handler = signal_handler_write;
  sigemptyset(&saw.sa_mask);
  saw.sa_flags = SA_RESTART;

  sad.sa_handler = signal_handler_delete;
  sigemptyset(&sad.sa_mask);
  sad.sa_flags = SA_RESTART;
  
  if (sigaction(SIGUSR1, &saw, NULL) == -1) {
    fprintf(stderr, "sigaction w error.\n");
  }
  // 3. signal terminate
  if (sigaction(SIGTERM, &sad, NULL) == -1) {
    fprintf(stderr, "sigaction w error.\n");
  }
  if (sigaction(SIGINT, &sad, NULL) == -1) {
    fprintf(stderr, "sigaction w error.\n");
  }
  fd_set master;
  fd_set read_fds;
  fdmax = server->listen_fd;
  FD_SET(server->listen_fd, &master);
  // connection and handle request
  while (1) {
  // find the idle worker, then designate works to it.
       fprintf(stderr, "Called select.\n");
       memset(&addr, 0, sizeof(addr));
       conn_len = 0;
       read_fds = master;
       if( select(fdmax+1, &read_fds, NULL, NULL, NULL) == -1 ){
          perror("select");
	  return -1;
       }
       fprintf(stderr, "Called select.\n");
       for( i = 0; i <= fdmax; i++ ){
	  if( FD_ISSET(i, &read_fds) ){
	      if( i == server->listen_fd ){
		conn_fd = accept(server->listen_fd, (struct sockaddr*)&addr, 
							(socklen_t*)&conn_len);
       		// waiting client connect
       		if (conn_fd < 0) {
         	  if (errno == ENFILE) {
           	    fprintf(stderr, "out of file descriptor table\n");
                    continue;
                } 
         	else if (errno == EAGAIN || errno == EINTR) {
           	    continue;
         	} else {
                   fprintf(stderr, "accept err\n");
           	  fprintf(stderr, "code: %s\n", strerror(errno));
           	  break;
                  }
                } else {
		    FD_SET(conn_fd, &master);
	            if( conn_fd > fdmax){
	              fdmax = conn_fd;
	            }
		}
	      } else {
       		// find idle worker
                for (j = 0; j < server->threads; j++) {
		  if (working_status[j] == -1 && task_fd[i] == 0) {
	            task_fd[i] = 1;
                    working_status[j] = i;
		    fprintf(stderr, "Working on conn_fd = %d.\n", working_status[j]);
	            break;
	          }
                }
		int all_busy = 1;
		for (j = 0; j < server->threads; j++) {
		  if (working_status[j] == -1) {
		    all_busy = 0;
		    break;
		  }
		}
                if (all_busy) {
	          fprintf(stderr, "Conn_fd %d return busy.\n", i);
	          handle_busy(i);
                }
	      }
	}  // FD_ISSET
    }  // trace fds table
  } // while loop
  //handle_request(server, conn_fd);
  pthread_exit(NULL);
  free(worker);
  free(thread);
  free(working_status);
  return 1;
}

void handle_busy(int conn_fd) {
  csiebox_protocol_header header1, header2;
  memset(&header1, 0, sizeof(header1));
  memset(&header2, 0, sizeof(header2));
  if (recv_message(conn_fd, &header1, sizeof(header1))) {
    switch (header1.req.op) {
      case CSIEBOX_PROTOCOL_OP_LOGIN:
        header2.res.op = CSIEBOX_PROTOCOL_OP_LOGIN;
	fprintf(stderr, "Received login, Server busy.\n");
  	break;
      case CSIEBOX_PROTOCOL_OP_SYNC_META:
        header2.res.op = CSIEBOX_PROTOCOL_OP_SYNC_META;
        fprintf(stderr, "Received sync meta, Server busy.\n");
	break;
      case CSIEBOX_PROTOCOL_OP_SYNC_END:
        header2.res.op = CSIEBOX_PROTOCOL_OP_SYNC_END;
	fprintf(stderr, "Received sync end, Server busy.\n");
        break;
      case CSIEBOX_PROTOCOL_OP_SYNC_FILE:
        header2.res.op = CSIEBOX_PROTOCOL_OP_SYNC_FILE;
	fprintf(stderr, "Received sync file, Server busy.\n");
        break;
      case CSIEBOX_PROTOCOL_OP_RM:
        header2.res.op = CSIEBOX_PROTOCOL_OP_RM;
	fprintf(stderr, "Received rm, Server busy.\n");
	break;
      default:
	fprintf(stderr, "Received unknow op %x, Server busy.\n", header1.req.op);
	break;
    }
    // send busy message back	  
    header2.res.magic = CSIEBOX_PROTOCOL_MAGIC_RES;
    header2.res.datalen = 0;
    header2.res.status = CSIEBOX_PROTOCOL_STATUS_BUSY;
    send_message(conn_fd, &header2, sizeof(header2));
  }
  return;
}

void csiebox_server_destroy(csiebox_server** server) {
  csiebox_server* tmp = *server;
  *server = 0;
  if (!tmp) {
    return;
  }
  close(tmp->listen_fd);
  free(tmp->client);
  free(tmp);
}

static int parse_arg(csiebox_server* server, int argc, char** argv) {
  if (argc < 2) {
    return 0;
  }
  FILE* file = fopen(argv[1], "r");
  if (!file) {
    return 0;
  }
  // daemonize
  if (argc == 3) {
    if (strcmp("-d", argv[2]) == 0)
      server->arg.daemonize = 1;
    else
      server->arg.daemonize = 0;
  }
  fprintf(stderr, "reading config...\n");
  size_t keysize = 20, valsize = 20;
  char* key = (char*)malloc(sizeof(char) * keysize);
  char* val = (char*)malloc(sizeof(char) * valsize);
  ssize_t keylen, vallen;
  int accept_config_total = 4;
  int accept_config[4] = {0, 0, 0, 0};
  while ((keylen = getdelim(&key, &keysize, '=', file) - 1) > 0) {
    key[keylen] = '\0';
    vallen = getline(&val, &valsize, file) - 1;
    val[vallen] = '\0';
    fprintf(stderr, "config (%zd, %s)=(%zd, %s)\n", keylen, key, vallen, val);
    if (strcmp("path", key) == 0) {
      if (vallen <= sizeof(server->arg.path)) {
        strncpy(server->arg.path, val, vallen);
        accept_config[0] = 1;
      }
    } else if (strcmp("account_path", key) == 0) {
      if (vallen <= sizeof(server->arg.account_path)) {
        strncpy(server->arg.account_path, val, vallen);
        accept_config[1] = 1;
      }
    } else if (strcmp("thread", key) == 0){
      if (vallen > 0){
      	server->threads = atoi(val);
      	accept_config[2] = 1;
      }
    } else if (strcmp("run_path", key) == 0) {
      if (vallen > 0) {
	strncpy(server->arg.run, val, vallen);
	accept_config[3] = 1;
      }
    }
  }
  free(key);
  free(val);
  fclose(file);
  int i, test = 1;
  for (i = 0; i < accept_config_total; ++i) {
    test = test & accept_config[i];
  }
  if (!test) {
    fprintf(stderr, "config error\n");
    return 0;
  }
  return 1;
}
// MP4
static void* handle_request(void *arg) {
  // For MP4 usage. 
  struct thread_data *thread;
  thread = (struct thread_data*)arg;
  int tid = thread->thread_id, conn_fd;
  while (1) {
    if (working_status[tid] != -1) {
      conn_fd = working_status[tid];
      fprintf(stderr, "Thread %ld: working status and conn_fd = %d.\n"
		  			, tid, working_status[tid]);
      csiebox_server* server = thread->server;
      csiebox_protocol_header header;
      memset(&header, 0, sizeof(header));
      //while (recv_message(conn_fd, &header, sizeof(header))) {
      if (recv_message(conn_fd, &header, sizeof(header))) {
        if (header.req.magic == CSIEBOX_PROTOCOL_MAGIC_REQ) {
	  switch (header.req.op) {
            case CSIEBOX_PROTOCOL_OP_LOGIN:
  	      fprintf(stderr, "login\n");
  	      csiebox_protocol_login req;
  	      if (complete_message_with_header(conn_fd, &header, &req)) {
  	        login(server, conn_fd, &req);
  	      }
  	      break;
  	      case CSIEBOX_PROTOCOL_OP_SYNC_META:
  	        fprintf(stderr, "sync meta\n");
  	        csiebox_protocol_meta meta;
  	        if (complete_message_with_header(conn_fd, &header, &meta)) {
  	          // send message tell client working
	          csiebox_protocol_header header2;
  	          memset(&header2, 0, sizeof(header2));
    	          header2.res.magic = CSIEBOX_PROTOCOL_MAGIC_RES;
     	          header2.res.datalen = 0;
                  header2.res.status = CSIEBOX_PROTOCOL_STATUS_MORE;
                  send_message(conn_fd, &header2, sizeof(header2));
	          sync_file(server, conn_fd, &meta);
  	        } else fprintf(stderr, "failed get complete.\n");
  	      break;
	      case CSIEBOX_PROTOCOL_OP_SYNC_END:
  	        fprintf(stderr, "sync end\n");
	        csiebox_protocol_header end;
  	        memset(&end, 0, sizeof(end));
    	        end.res.magic = CSIEBOX_PROTOCOL_MAGIC_RES;
     	        end.res.datalen = 0;
                end.res.status = CSIEBOX_PROTOCOL_STATUS_OK;
                send_message(conn_fd, &end, sizeof(end));
  	      break;
  	      case CSIEBOX_PROTOCOL_OP_RM:
  	        fprintf(stderr, "rm\n");
  	        csiebox_protocol_rm rm;
  	        if (complete_message_with_header(conn_fd, &header, &rm)) {
	          csiebox_protocol_header header2;
  	          memset(&header2, 0, sizeof(header2));
    	          header2.res.magic = CSIEBOX_PROTOCOL_MAGIC_RES;
     	          header2.res.datalen = 0;
                  header2.res.status = CSIEBOX_PROTOCOL_STATUS_MORE;
                  send_message(conn_fd, &header2, sizeof(header2));
  	          rm_file(server, conn_fd, &rm);
  	        }
  	      break;
  	      default:
  	        fprintf(stderr, "unknow op %x\n", header.req.op);
  	        break;
  	  } // switch    
        }// magic
      } // revieve
      working_status[tid] = -1;
      task_fd[conn_fd] = 0; 
    }  // check working status
    else
      continue;
  }//  while loop
  fprintf(stderr, "conn_fd %d exit from thread %d.\n", conn_fd, tid);
  pthread_exit(NULL);
}

static int get_account_info(
  csiebox_server* server,  const char* user, csiebox_account_info* info) {
  FILE* file = fopen(server->arg.account_path, "r");
  if (!file) {
    return 0;
  }
  size_t buflen = 100;
  char* buf = (char*)malloc(sizeof(char) * buflen);
  memset(buf, 0, buflen);
  ssize_t len;
  int ret = 0;
  int line = 0;
  while ((len = getline(&buf, &buflen, file) - 1) > 0) {
    ++line;
    buf[len] = '\0';
    char* u = strtok(buf, ",");
    if (!u) {
      fprintf(stderr, "ill form in account file, line %d\n", line);
      continue;
    }
    if (strcmp(user, u) == 0) {
      memcpy(info->user, user, strlen(user));
      char* passwd = strtok(NULL, ",");
      if (!passwd) {
        fprintf(stderr, "ill form in account file, line %d\n", line);
        continue;
      }
      md5(passwd, strlen(passwd), info->passwd_hash);
      ret = 1;
      break;
    }
  }
  free(buf);
  fclose(file);
  return ret;
}

static void login(csiebox_server* server, int conn_fd, csiebox_protocol_login* login) {
  fprintf(stderr, "login now.\n");
  int succ = 1;
  csiebox_client_info* info = (csiebox_client_info*)malloc(sizeof(csiebox_client_info));
  memset(info, 0, sizeof(csiebox_client_info));
  if (!get_account_info(server, login->message.body.user, &(info->account))) {
    fprintf(stderr, "cannot find account\n");
    succ = 0;
  }
  if (succ &&
      memcmp(login->message.body.passwd_hash,
             info->account.passwd_hash,
             MD5_DIGEST_LENGTH) != 0) {
    fprintf(stderr, "passwd miss match\n");
    succ = 0;
  }

  csiebox_protocol_header header;
  memset(&header, 0, sizeof(header));
  header.res.magic = CSIEBOX_PROTOCOL_MAGIC_RES;
  header.res.op = CSIEBOX_PROTOCOL_OP_LOGIN;
  header.res.datalen = 0;
  if (succ) {
    if (server->client[conn_fd]) {
      free(server->client[conn_fd]);
    }
    info->conn_fd = conn_fd;
    server->client[conn_fd] = info;
    header.res.status = CSIEBOX_PROTOCOL_STATUS_OK;
    header.res.client_id = info->conn_fd;
    char* homedir = get_user_homedir(server, info);
    mkdir(homedir, DIR_S_FLAG);
    free(homedir);
  } else {
    header.res.status = CSIEBOX_PROTOCOL_STATUS_FAIL;
    free(info);
  }
  fprintf(stderr, "Send login info to client.\n");
  send_message(conn_fd, &header, sizeof(header));
}

static void logout(csiebox_server* server, int conn_fd) {
  free(server->client[conn_fd]);
  server->client[conn_fd] = 0;
  close(conn_fd);
}

static void sync_file(csiebox_server* server, int conn_fd, csiebox_protocol_meta* meta) {
  csiebox_client_info* info = server->client[conn_fd];
  char* homedir = get_user_homedir(server, info);
  printf("homedir = %s\n", homedir);
  char buf[PATH_MAX], req_path[PATH_MAX];
  memset(buf, 0, PATH_MAX);
  memset(req_path, 0, PATH_MAX);
  recv_message(conn_fd, buf, meta->message.body.pathlen);
  sprintf(req_path, "%s%s", homedir, buf);
  free(homedir);
  fprintf(stderr, "req_path: %s\n", req_path);
  struct stat stat;
  memset(&stat, 0, sizeof(struct stat));
  int need_data = 0, change = 0;
  if (lstat(req_path, &stat) < 0) {
    need_data = 1;
    change = 1;
  } else { 					
    if(stat.st_mode != meta->message.body.stat.st_mode) { 
      chmod(req_path, meta->message.body.stat.st_mode);
    }				
    if(stat.st_atime != meta->message.body.stat.st_atime ||
       stat.st_mtime != meta->message.body.stat.st_mtime){
      struct utimbuf* buf = (struct utimbuf*)malloc(sizeof(struct utimbuf));
      buf->actime = meta->message.body.stat.st_atime;
      buf->modtime = meta->message.body.stat.st_mtime;
      if(utime(req_path, buf)!=0){
        printf("time fail\n");
      }
    }
    uint8_t hash[MD5_DIGEST_LENGTH];
    memset(hash, 0, MD5_DIGEST_LENGTH);
    if ((stat.st_mode & S_IFMT) == S_IFDIR) {
    } else {
      md5_file(req_path, hash);
    }
    if (memcmp(hash, meta->message.body.hash, MD5_DIGEST_LENGTH) != 0) {
      need_data = 1;
    }
  }

  csiebox_protocol_header header;
  memset(&header, 0, sizeof(header));
  header.res.magic = CSIEBOX_PROTOCOL_MAGIC_RES;
  header.res.op = CSIEBOX_PROTOCOL_OP_SYNC_META;
  header.res.datalen = 0;
  header.res.client_id = conn_fd;
  if (need_data) {
    header.res.status = CSIEBOX_PROTOCOL_STATUS_MORE;
  } else {
    header.res.status = CSIEBOX_PROTOCOL_STATUS_OK;
  }
  send_message(conn_fd, &header, sizeof(header));
  
  if (need_data) {
    csiebox_protocol_file file;
    memset(&file, 0, sizeof(file));
    recv_message(conn_fd, &file, sizeof(file));
    fprintf(stderr, "sync file: %zd\n", file.message.body.datalen);
    if ((meta->message.body.stat.st_mode & S_IFMT) == S_IFDIR) {
      fprintf(stderr, "dir\n");
      mkdir(req_path, DIR_S_FLAG);
    } else {
      fprintf(stderr, "regular file\n");
      int fd = open(req_path, O_CREAT | O_WRONLY | O_TRUNC, REG_S_FLAG);
      size_t total = 0, readlen = 0;;
      char buf[FILE_SIZE];
      memset(buf, 0, FILE_SIZE);
      //char buf[FILE_SIZE];
      //memset(buf, 0, FILE_SIZE);
      while (file.message.body.datalen > total) {
        if (file.message.body.datalen - total < FILE_SIZE) {
          readlen = file.message.body.datalen - total;
        } else {
          readlen = FILE_SIZE;
        }
        if (!recv_message(conn_fd, buf, readlen)) {
          fprintf(stderr, "file broken\n");
          break;
        }
        total += readlen;
        if (fd > 0) {
          write(fd, buf, readlen);
        }
      }
      //lock.l_type = F_UNLCK;
      //fcntl(fd, F_SETLKW, &lock);
      if (fd > 0) {
        close(fd);
      }
    }
    if (change) {
      chmod(req_path, meta->message.body.stat.st_mode);
      struct utimbuf* buf = (struct utimbuf*)malloc(sizeof(struct utimbuf));
      buf->actime = meta->message.body.stat.st_atime;
      buf->modtime = meta->message.body.stat.st_mtime;
      utime(req_path, buf);
    }
    header.res.op = CSIEBOX_PROTOCOL_OP_SYNC_FILE;
    header.res.status = CSIEBOX_PROTOCOL_STATUS_OK;
    send_message(conn_fd, &header, sizeof(header));
  }
  fprintf(stderr, "exit from sync meta\n");
}

static char* get_user_homedir(csiebox_server* server, csiebox_client_info* info) {
  char* ret = (char*)malloc(sizeof(char) * PATH_MAX);
  memset(ret, 0, PATH_MAX);
  sprintf(ret, "%s/%s", server->arg.path, info->account.user);
  return ret;
}

static void rm_file(csiebox_server* server, int conn_fd, csiebox_protocol_rm* rm) {
  csiebox_client_info* info = server->client[conn_fd];
  char* homedir = get_user_homedir(server, info);
  char req_path[PATH_MAX], buf[PATH_MAX];
  memset(req_path, 0, PATH_MAX);
  memset(buf, 0, PATH_MAX);
  recv_message(conn_fd, buf, rm->message.body.pathlen);
  sprintf(req_path, "%s%s", homedir, buf);
  free(homedir);
  fprintf(stderr, "rm (%zd, %s)\n", strlen(req_path), req_path);
  struct stat stat;
  memset(&stat, 0, sizeof(stat));
  lstat(req_path, &stat);
  if ((stat.st_mode & S_IFMT) == S_IFDIR) {
    rmdir(req_path);
  } else {
    unlink(req_path);
  }

  csiebox_protocol_header header;
  memset(&header, 0, sizeof(header));
  header.res.magic = CSIEBOX_PROTOCOL_MAGIC_RES;
  header.res.op = CSIEBOX_PROTOCOL_OP_RM;
  header.res.datalen = 0;
  header.res.client_id = conn_fd;
  header.res.status = CSIEBOX_PROTOCOL_STATUS_OK;
  send_message(conn_fd, &header, sizeof(header));
}
