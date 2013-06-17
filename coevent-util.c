#include "coevent.h"

int coevent_setnonblocking(int fd){
	int opts;
	opts=fcntl(fd, F_GETFL);
	if (opts < 0)
		return 0;

	opts = opts | O_NONBLOCK;
	if (fcntl(fd, F_SETFL, opts) < 0)
		return 0;

	return 1;
}

uint32_t fnv1a_32(const char *data, uint32_t len) {
	uint32_t rv = 0x811c9dc5U;
	uint32_t i;
	for (i = 0; i < len; i++) {
		rv = (rv ^ (unsigned char)data[i]) * 16777619;
	}
	return rv;
}

uint32_t fnv1a_64(const char *data, uint32_t len) {
	uint64_t rv = 0xcbf29ce484222325UL;
	uint32_t i;
	for (i = 0; i < len; i++) {
		rv = (rv ^ (unsigned char)data[i]) * 1099511628211UL;
	}
	return (uint32_t)rv;
}

void *connect_pool_p[2][64] = {{NULL32 NULL32},{NULL32 NULL32}};
int connect_pool_ttl = 30;
int get_connection_in_pool(int epoll_fd, unsigned long pool_key){
	//time(&timer);
	int p = (timer/connect_pool_ttl)%2;
	cosocket_connection_pool_t *n = NULL, *m = NULL, *nn = NULL;
	/// clear old caches
	int q = (p+1)%2;
	int i = 0;
	struct epoll_event ev;
	for(i = 0; i < 64; i++){
		n = connect_pool_p[q][i];
		while(n){
			m = n;
			n = n->next;
			if(m->recached == 0){
				m->recached = 1;
				nn = connect_pool_p[p][m->pool_key%64];
				if(nn == NULL){
					connect_pool_p[p][m->pool_key%64] = m;
					m->next = NULL;
					m->uper = NULL;
				}else{
					m->uper = NULL;
					m->next = nn;
					nn->uper = m;
					connect_pool_p[p][m->pool_key%64] = m;
				}
			}else{
				epoll_ctl(epoll_fd, EPOLL_CTL_DEL, m->fd, &ev);
				close(m->fd);
				free(m);
			}
		}
		connect_pool_p[q][i] = NULL;
	}

	/// end
	if(pool_key == 0)return -1; /// only do clear job
	int k = pool_key%64;
	
	n = connect_pool_p[p][k];
	while(n != NULL){
		if(n->pool_key == pool_key){
			break;
		}
		n = (cosocket_connection_pool_t*)n->next;
	}
	
	if(n){
		if(n == connect_pool_p[p][k]){ /// at top
			m = n->next;
			if(m){
				m->uper = NULL;
				connect_pool_p[p][k] = m;
			}else connect_pool_p[p][k] = NULL;
		}else{
			((cosocket_connection_pool_t*)n->uper)->next = n->next;
			if(n->next)
				((cosocket_connection_pool_t*)n->next)->uper = n->uper;
		}
		
		int fd = n->fd;
		free(n);
		//printf("get fd in pool%d %d key:%d\n",p, fd, k);
		return fd;
	}
	
	return -1;
}

void del_connection_in_pool(int epoll_fd, cosocket_connection_pool_t* n){
	int k = n->pool_key%64;
	if(n == connect_pool_p[0][k]){
		connect_pool_p[0][k] = n->next;
		if(n->next){
			((cosocket_connection_pool_t*)n->next)->uper = NULL;
		}
	}else if(n == connect_pool_p[1][k]){
		connect_pool_p[1][k] = n->next;
		if(n->next){
			((cosocket_connection_pool_t*)n->next)->uper = NULL;
		}
	}else{
		((cosocket_connection_pool_t*)n->uper)->next = n->next;
		if(n->next)
			((cosocket_connection_pool_t*)n->next)->uper = n->uper;
	}
	
	struct epoll_event ev;
	epoll_ctl(epoll_fd, EPOLL_CTL_DEL, n->fd, &ev);
	close(n->fd);
	free(n);
}

int add_connection_to_pool(int epoll_fd, unsigned long pool_key, int pool_size, int fd){
	if(pool_key < 0)return -1;
	time(&timer);
	int p = (timer/connect_pool_ttl)%2;
	struct epoll_event ev;
	
	cosocket_connection_pool_t *n = NULL, *m = NULL;
	
	int k = pool_key%64;
	
	n = connect_pool_p[p][k];
	
	if(n == NULL){
		m = malloc(sizeof(cosocket_connection_pool_t));
		if(m == NULL)
			return 0;
		m->type = EPOLL_PTR_TYPE_COSOCKET_WAIT;
		m->recached = 0;
		m->pool_key = pool_key;
		m->next = NULL;
		m->uper = NULL;
		m->fd = fd;
	
		connect_pool_p[p][k] = m;
		
		ev.data.ptr = m;
		ev.events = EPOLLIN;
		if (epoll_ctl(epoll_fd, EPOLL_CTL_MOD, m->fd, &ev) == -1)
			printf("EPOLL_CTL_MOD error: %d %s", __LINE__, strerror(errno));
		
		return 1;
	}else{
		int in_pool = 0;
		while(n != NULL){
			if(n->pool_key == pool_key){
				if(in_pool++ >= pool_size)
					return 0;
			}

			if(n->next == NULL){ /// last
				m = malloc(sizeof(cosocket_connection_pool_t));
				if(m == NULL)
					return 0;
				m->type = EPOLL_PTR_TYPE_COSOCKET_WAIT;
				m->recached = 0;
				m->pool_key = pool_key;
				m->next = NULL;
				m->uper = n;
				m->fd = fd;
	
				n->next = m;
				
				ev.data.ptr = m;
				ev.events = EPOLLIN;
				if (epoll_ctl(epoll_fd, EPOLL_CTL_MOD, m->fd, &ev) == -1)
					printf("EPOLL_CTL_MOD error: %d %s", __LINE__, strerror(errno));
		
				return 1;
			}
			n = (cosocket_connection_pool_t*)n->next;
		}
	}
}

void *dns_cache[3][64] = {{NULL32 NULL32},{NULL32 NULL32},{NULL32 NULL32}};
int dns_cache_ttl = 180;
int get_dns_cache(const char *name, struct in_addr *addr){
	//time(&timer);
	int p = (timer/dns_cache_ttl)%3;
	dns_cache_item_t *n = NULL, *m = NULL;
	/// clear old caches
	int q = (p+2)%3;
	int i = 0;
	for(i = 0; i < 64; i++){
		n = dns_cache[q][i];
		while(n){
			m = n;
			n = n->next;
			free(m);
		}
		dns_cache[q][i] = NULL;
	}
	/// end
	int nlen = strlen(name);
	uint32_t key1 = fnv1a_32(name, nlen);
	uint32_t key2 = fnv1a_64(name, nlen);
	
	n = dns_cache[p][key1%64];
	while(n != NULL){
		if(n->key1 == key1 && n->key2 == key2)
			break;
		n = (dns_cache_item_t*)n->next;
	}
	if(n){
		memcpy(addr, &n->addr, sizeof(struct in_addr));
		if(n->recached != 1){
			n->recached = 1;
			add_dns_cache(name, n->addr, 1);
		}
		return 1;
	}

	return 0;
}

void add_dns_cache(const char *name, struct in_addr addr, int do_recache){
	time(&timer);
	int p = (timer/dns_cache_ttl)%3;
	if(do_recache==1)
		p = (p+1)%3;
	dns_cache_item_t *n = NULL, *m = NULL;
	int nlen = strlen(name);
	uint32_t key1 = fnv1a_32(name, nlen);
	uint32_t key2 = fnv1a_64(name, nlen);
	
	int k = key1%64;
	n = dns_cache[p][k];
	if(n == NULL){
		m = malloc(sizeof(dns_cache_item_t));
		if(m == NULL)
			return;
		m->key1 = key1;
		m->key2 = key2;
		m->next = NULL;
		m->recached = do_recache;
		memcpy(&m->addr, &addr, sizeof(struct in_addr));
	
		dns_cache[p][k] = m;
	}else{
		while(n != NULL){
			if(n->key1 == key1 && n->key2 == key2)
				return; /// exists
			if(n->next == NULL){ /// last
				m = malloc(sizeof(dns_cache_item_t));
				if(m == NULL)
					return;
				m->key1 = key1;
				m->key2 = key2;
				m->next = NULL;
				m->recached = do_recache;
				memcpy(&m->addr, &addr, sizeof(struct in_addr));
	
				n->next = m;
				return;
			}
			n = (dns_cache_item_t*)n->next;
		}
	}
}

uint16_t dns_tid = 0;
struct sockaddr_in dns_servers[4];
int dns_server_count = 0;
char pkt[2048];
int do_dns_query(int epoll_fd, cosocket_t *cok, const char *name){
	if(dns_server_count == 0){
		int p1 = 0, p2 = 0, p3 = 0, p4 = 0, i = 0;
		for(i=0;i<4;i++){
			dns_servers[i].sin_family = AF_INET;
			dns_servers[i].sin_port = htons(53);
		}
		FILE *fp;
		char line[200] , *p;
		if((fp = fopen("/etc/resolv.conf" , "r")) == NULL)
		{
			printf("Failed opening /etc/resolv.conf file \n");
		}else{
			while(fgets(line , 200 , fp))
			{
				if(line[0] == '#')
				{
					continue;
				}
				if(strncmp(line , "nameserver" , 10) == 0)
				{
					p = strtok(line , " ");
					p = strtok(NULL , " ");
					
					//p now is the dns ip :)
					if(sscanf(p, "%d.%d.%d.%d", &p1, &p2, &p3, &p4) == 4){
						dns_servers[dns_server_count].sin_addr.s_addr = htonl(((p1 << 24) | (p2 << 16) | (p3 << 8) | (p4)));
						dns_server_count ++;
					}
					if(dns_server_count > 1)break;
				}
			}
			fclose(fp);
		}
		if(dns_server_count < 2){
			dns_servers[dns_server_count].sin_addr.s_addr = inet_addr("8.8.8.8");
			dns_server_count++;
		}

		if(dns_server_count < 2){
			dns_servers[dns_server_count].sin_addr.s_addr = inet_addr("208.67.22.222");
			dns_server_count++;
		}
	}
	dns_tid += 1;
	if(dns_tid > 65535-1)dns_tid = 1;
	
	struct epoll_event ev;
	cok->dns_query_fd = socket(PF_INET, SOCK_DGRAM, 17);

	if(cok->dns_query_fd < 0)
		return 0;
	cok->dns_tid = dns_tid;
	int nlen = strlen(name);
	if(nlen < 60){
		memcpy(cok->dns_query_name, name, nlen);
		cok->dns_query_name[nlen] = '\0';
	}else{
		cok->dns_query_name[0] = '-';
		cok->dns_query_name[1] = '\0';
		nlen = 1;
	}
	name = cok->dns_query_name;
	
	int opt = 1;
	ioctl(cok->dns_query_fd, FIONBIO, &opt);

	ev.events = EPOLLIN | EPOLLET;
	ev.data.fd = cok->dns_query_fd;
	ev.data.ptr = cok;
	epoll_ctl(epoll_fd, EPOLL_CTL_ADD, cok->dns_query_fd, &ev);
	
	int	i, n, m;
	dns_query_header_t *header = NULL;

	const char *s;
	char *p;

	header			 = (dns_query_header_t*)pkt;
	header->tid		 = dns_tid;
	header->flags	 = htons(0x100); 
	header->nqueries = htons(1);
	header->nanswers = 0;
	header->nauth	 = 0;
	header->nother	 = 0;

	// Encode DNS name 
	p = (char *) &header->data;	/* For encoding host name into packet */

	do{
		if ((s = strchr(name, '.')) == NULL)
			s = name + nlen;

		n = s - name;			/* Chunk length */
		*p++ = n;				/* Copy length */
		for (i = 0; i < n; i++)	/* Copy chunk */
			*p++ = name[i];

		if (*s == '.')
			n++;

		name += n;
		nlen -= n;

	} while (*s != '\0');

	*p++ = 0;			/* Mark end of host name */
	*p++ = 0;			/* Well, lets put this byte as well */
	*p++ = 1;			/* Query Type */

	*p++ = 0;
	*p++ = 1;			/* Class: inet, 0x0001 */

	n = p - pkt;		/* Total packet length */

	sendto(cok->dns_query_fd, pkt, n, 0, (struct sockaddr *) &dns_servers[(cok->dns_query_fd+1)%dns_server_count], sizeof(struct sockaddr));
	if((m = sendto(cok->dns_query_fd, pkt, n, 0, (struct sockaddr *) &dns_servers[cok->dns_query_fd%dns_server_count], sizeof(struct sockaddr))) != n)
	{
		return 0;
	}
	return 1;
}

#define	NTOHS(p)	(((p)[0] << 8) | (p)[1])
void parse_dns_result(int epoll_fd, int fd, cosocket_t *cok, const unsigned char *pkt, int len){
	struct epoll_event ev;
	epoll_ctl(epoll_fd, EPOLL_CTL_DEL, fd, &ev);
	
	const unsigned char	*p, *e, *s;
	dns_query_header_t *header = NULL;

	uint16_t type;
	int found = 0, stop, dlen, nlen, i;
	int err = 0;

	header = (dns_query_header_t*) pkt;
	if (ntohs(header->nqueries) != 1)
		err = 1;

	if(header->tid != cok->dns_tid)
		err = 1;

	/* Skip host name */
	if(err == 0)
		for (e = pkt + len, nlen = 0, s = p = &header->data[0];
			p < e && *p != '\0'; p++)
			nlen++;

	/* We sent query class 1, query type 1 */
	if (&p[5] > e || NTOHS(p + 1) != 0x01)
		err = 1;

	struct in_addr ips[10];
	/* Go to the first answer section */
	if(err == 0){
		p += 5;
		/* Loop through the answers, we want A type answer */
		for (found = stop = 0; !stop && &p[12] < e; ) {

			/* Skip possible name in CNAME answer */
			if (*p != 0xc0) {
				while (*p && &p[12] < e)
					p++;
				p--;
			}

			type = htons(((uint16_t *)p)[1]);

			if (type == 5) 
			{
				/* CNAME answer. shift to the next section */
				dlen = htons(((uint16_t *) p)[5]);
				p += 12 + dlen;
			} 
			else if (type == 0x01) 
			{

				dlen = htons(((uint16_t *) p)[5]);
				p += 12;

				if (p + dlen <= e) 
				{
					memcpy(&ips[found], p, dlen); 
				}
				p += dlen;
				if(++found == header->nanswers) stop = 1;
				if(found>=10)break;
			}
			else
			{
				stop = 1;
			}
		}
	}

	if(found > 0){
		cok->addr.sin_addr= ips[cok->dns_query_fd%found];
		int sockfd = get_connection_in_pool(epoll_fd, cok->pool_key);
		if(sockfd != -1){
			cok->fd = sockfd;
			cok->status = 2;

			lua_pushboolean(cok->L, 1);
			int ret = lua_resume(cok->L, 1);
			if (ret == LUA_ERRRUN && lua_isstring(cok->L, -1)) {
				printf("%s,%d isstring: %s\n", __FILE__,__LINE__, lua_tostring(cok->L, -1));
				lua_pop(cok->L, -1);
			}
			cok->reusedtimes = 1;
			return;
		}
		
		if((sockfd=socket(AF_INET,SOCK_STREAM,0))<0){
			lua_pushnil(cok->L);
			lua_pushstring(cok->L, "Init socket error!");
			int ret = lua_resume(cok->L, 2);
			if (ret == LUA_ERRRUN && lua_isstring(cok->L, -1)) {
				printf("%s:%d isstring: %s\n", __FILE__,__LINE__, lua_tostring(cok->L, -1));
				lua_pop(cok->L, -1);
			}
			return;
		}
		cok->fd = sockfd;
		if(!coevent_setnonblocking(cok->fd)){
			close(cok->fd);
			lua_pushnil(cok->L);
			lua_pushstring(cok->L, "Init socket error!");
			int ret = lua_resume(cok->L, 2);
			if (ret == LUA_ERRRUN && lua_isstring(cok->L, -1)) {
				printf("%s:%d isstring: %s\n", __FILE__,__LINE__, lua_tostring(cok->L, -1));
				lua_pop(cok->L, -1);
			}
			return;
		}
		cok->reusedtimes = 0;
		
		ev.data.ptr = cok;
		ev.events = EPOLLOUT;
		epoll_ctl(epoll_fd, EPOLL_CTL_ADD, cok->fd, &ev);
		
		add_dns_cache(cok->dns_query_name, cok->addr.sin_addr, 0);
		
		int ret = connect(cok->fd,(struct sockaddr*)&cok->addr,sizeof(struct sockaddr));
		if(ret == 0){
			///////// !!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!! connected /////
			lua_pushboolean(cok->L, 1);
			int ret = lua_resume(cok->L, 1);
			if (ret == LUA_ERRRUN && lua_isstring(cok->L, -1)) {
				printf("%s:%d isstring: %s\n", __FILE__,__LINE__, lua_tostring(cok->L, -1));
				lua_pop(cok->L, -1);
			}
			
		}
		return;
	}
	
	{
		epoll_ctl(epoll_fd, EPOLL_CTL_DEL, cok->fd, &ev);
		close(cok->fd);
		cok->fd = -1;
		cok->status = 0;
		lua_pushnil(cok->L);
		lua_pushstring(cok->L, "names lookup error!");
		int ret = lua_resume(cok->L, 2);
		if (ret == LUA_ERRRUN && lua_isstring(cok->L, -1)) {
			printf("%s:%d isstring: %s\n", __FILE__,__LINE__, lua_tostring(cok->L, -1));
			lua_pop(cok->L, -1);
		}
	}
}

static struct hostent* localhost_ent = NULL;
int tcp_connect(const char *host, int port, cosocket_t *cok, int epoll_fd, int *ret){
	int sockfd = -1;

	bzero(&cok->addr,sizeof(struct sockaddr_in));
	if(port > 0){
		cok->addr.sin_family = AF_INET;
		cok->addr.sin_port=htons(port);
		cok->addr.sin_addr.s_addr=inet_addr(host);//按IP初始化
	
		if(cok->addr.sin_addr.s_addr == INADDR_NONE){//如果输入的是域名
			int is_localhost = (strcmp(host,"localhost")==0);
			struct hostent *phost = localhost_ent;
			int in_cache = 0;
			if(!is_localhost || localhost_ent == NULL){
				if(is_localhost)
					phost = (struct hostent*)gethostbyname(host);
				else{
					if(get_dns_cache(host, &cok->addr.sin_addr)){
						in_cache = 1;
					}else{
						cok->fd = -1;
						if(!do_dns_query(epoll_fd, cok, host)){
							return -3;
						}
						add_to_timeout_link(cok, cok->timeout/2);
						*ret = EINPROGRESS;
						return sockfd;
					}
				}
				if(is_localhost){
					if(localhost_ent==NULL)localhost_ent = malloc(sizeof(struct hostent));// size 32 ,and sizeof(cosocket_swop_t) == 32
					memcpy(localhost_ent, phost, sizeof(struct hostent));
				}
			}

			if(in_cache == 0){
				if(phost==NULL){
					close(sockfd);
					return -2;
				}
				
				cok->addr.sin_addr.s_addr =((struct in_addr*)phost->h_addr)->s_addr;
			}
		}
	}
	sockfd = get_connection_in_pool(epoll_fd, cok->pool_key);
	if(sockfd == -1){
		if((sockfd = socket(port > 0 ? AF_INET:AF_UNIX, SOCK_STREAM, 0)) < 0){
			return -1;
		}
		if(!coevent_setnonblocking(sockfd)){
			close(sockfd);
			return -1;
		}
		cok->reusedtimes = 0;
	}else{
		cok->reusedtimes = 1;
		*ret = 0;
		return sockfd;
	}
	
	struct epoll_event ev;
	cok->fd = sockfd;
	ev.data.ptr = cok;
	ev.events = EPOLLOUT;
	epoll_ctl(epoll_fd, EPOLL_CTL_ADD, sockfd, &ev);
	
	add_to_timeout_link(cok, cok->timeout/2);
	if(port > 0)
		*ret = connect(sockfd,(struct sockaddr*)&cok->addr, sizeof(struct sockaddr_in));
	else{ /// connect to unix domain socket
		struct sockaddr_un un;
		memset(&un, 0, sizeof(struct sockaddr_un));
		strcpy(un.sun_path, host);
		un.sun_family = AF_UNIX;
		int length = offsetof(struct sockaddr_un,sun_path)+strlen(un.sun_path);
		*ret = connect(sockfd, (struct sockaddr *)&un, length);
	}

	return sockfd;
}

static void *timeout_links[64] = {NULL32 NULL32};
int add_to_timeout_link(cosocket_t *cok, int timeout){
	int p = ((long)cok)%64;
	timeout_link_t *_tl = NULL, *_tll = NULL, *_ntl = NULL;
	int add = 0;
	if(timeout < 10)timeout = 1000;
	if(timeout_links[p] == NULL){
		_ntl = malloc(sizeof(timeout_link_t));
		if(_ntl == NULL)return 0;
		_ntl->cok = cok;
		_ntl->uper = NULL;
		_ntl->next = NULL;
		_ntl->timeout = longtime()+timeout;
		timeout_links[p] = _ntl;
		
		return 1;
	}else{
		add = 1;
		_tl = timeout_links[p];
		while(_tl){
			_tll = _tl; /// get last item
			if(_tl->cok == cok){
				add = 0;
				break;
			}
			_tl = _tl->next;
		}
		
		if(_tll != NULL){
			_ntl = malloc(sizeof(timeout_link_t));
			if(_ntl == NULL)return 0;
			_ntl->cok = cok;
			_ntl->uper = _tll;
			_ntl->next = NULL;
			_ntl->timeout = longtime()+timeout;
			
			_tll->next = _ntl;
			return 1;
		}
	}
	
	return 0;
}

int del_in_timeout_link(cosocket_t *cok){
	int p = ((long)cok)%64;
	timeout_link_t *_tl = NULL, *_utl = NULL, *_ntl = NULL;
	if(timeout_links[p] == NULL){
		return 0;
	}else{
		_tl = timeout_links[p];
		while(_tl){
			if(_tl->cok == cok){
				_utl = _tl->uper;
				_ntl = _tl->next;
				if(_utl == NULL){
					timeout_links[p] = _ntl;
					if(_ntl != NULL)
						_ntl->uper = NULL;
				}else{
					_utl->next = _tl->next;
					if(_ntl != NULL)
						_ntl->uper = _utl;
				}
				
				free(_tl);
				return 1;
			}
			_tl = _tl->next;
		}
	}
	
	return 0;
}

int chk_do_timeout_link(int epoll_fd){
	long nt = longtime();
	timeout_link_t *_tl = NULL, *_ttl = NULL, *_utl = NULL, *_ntl = NULL;
	struct epoll_event ev;
	
	int i = 0;
	for(i=0; i < 64; i++){
		if(timeout_links[i] == NULL)
			continue;
		
		_tl = timeout_links[i];
		while(_tl){
			_ttl = _tl->next;
			if(nt >= _tl->timeout){
				_utl = _tl->uper;
				_ntl = _tl->next;
				if(_utl == NULL){
					timeout_links[i] = _ntl;
					if(_ntl != NULL)
						_ntl->uper = NULL;
				}else{
					_utl->next = _tl->next;
					if(_ntl != NULL)
						_ntl->uper = _utl;
				}
				
				cosocket_t *cok = _tl->cok;
				free(_tl);
				
				//printf("fd timeout %d %d %ld\n", cok->fd,cok->dns_query_fd, _tl->timeout);
				
				if(cok->dns_query_fd > -1){
					epoll_ctl(epoll_fd, EPOLL_CTL_DEL, cok->dns_query_fd, &ev);
					close(cok->dns_query_fd);
					cok->dns_query_fd = -1;
				}
				
				{
					epoll_ctl(epoll_fd, EPOLL_CTL_DEL, cok->fd, &ev);
					close(cok->fd);
					cok->fd = -1;
					cok->status = 0;
				}
				
				lua_pushnil(cok->L);
				lua_pushstring(cok->L, "timeout!");
				int ret = lua_resume(cok->L, 2);
				if (ret == LUA_ERRRUN && lua_isstring(cok->L, -1)) {
					//printf("%s:%d isstring: %s\n", __FILE__,__LINE__, lua_tostring(cok->L, -1));
					if(lua_gettop(cok->L) > 1){
						lua_replace(cok->L, 2);
						lua_pushnil(cok->L);
						lua_replace(cok->L, 1);
						lua_settop(cok->L, 2);
					}else{
						lua_pushnil(cok->L);
						lua_replace(cok->L, 1);
					}

					lua_f_coroutine_resume_waiting(cok->L);
				}
			}
			_tl = _ttl;
		}
	}
}

long longtime(){
	struct timeb t;
	ftime(&t);
	return 1000 * t.time + t.millitm;
}

int lua_f_time(lua_State *L){
	lua_pushnumber(L, time(NULL));
	return 1;
}

int lua_f_longtime(lua_State *L){
	struct timeb t;
	ftime(&t);
	lua_pushnumber(L, 1000 * t.time + t.millitm);
	return 1;
}

size_t lua_calc_strlen_in_table(lua_State *L, int index, int arg_i, unsigned strict){
	double key;
	int max;
	int i;
	int type;
	size_t size;
	size_t len;
	const char *msg;

	if (index < 0) {
		index = lua_gettop(L) + index + 1;
	}

	max = 0;

	lua_pushnil(L); /* stack: table key */
	while (lua_next(L, index) != 0) { /* stack: table key value */
		if (lua_type(L, -2) == LUA_TNUMBER) {

			key = lua_tonumber(L, -2);

			if (floor(key) == key && key >= 1) {
				if (key > max) {
					max = key;
				}

				lua_pop(L, 1); /* stack: table key */
				continue;
			}
		}

		/* not an array (non positive integer key) */
		lua_pop(L, 2); /* stack: table */

		//msg = lua_pushfstring(L, "non-array table found"); /// commented by oneoo
		//luaL_argerror(L, arg_i, msg);
		return 0;
	}

	size = 0;

	for (i = 1; i <= max; i++) {
		lua_rawgeti(L, index, i); /* stack: table value */
		type = lua_type(L, -1);

		switch (type) {
			case LUA_TNUMBER:
			case LUA_TSTRING:

				lua_tolstring(L, -1, &len);
				size += len;
				break;

			case LUA_TNIL:

				if (strict) {
					goto bad_type;
				}

				size += sizeof("nil") - 1;
				break;

			case LUA_TBOOLEAN:

				if (strict) {
					goto bad_type;
				}

				if (lua_toboolean(L, -1)) {
					size += sizeof("true") - 1;

				} else {
					size += sizeof("false") - 1;
				}

				break;

			case LUA_TTABLE:

				size += lua_calc_strlen_in_table(L, -1, arg_i, strict);
				break;

			case LUA_TLIGHTUSERDATA:

				if (strict) {
					goto bad_type;
				}

				if (lua_touserdata(L, -1) == NULL) {
					size += sizeof("null") - 1;
					break;
				}

				continue;

			default:

bad_type:
				msg = lua_pushfstring(L, "bad data type %s found", lua_typename(L, type));
				return luaL_argerror(L, arg_i, msg);
		}

		lua_pop(L, 1); /* stack: table */
	}

	return size;
}

unsigned char *lua_copy_str_in_table(lua_State *L, int index, u_char *dst){
	double key;
	int max;
	int i;
	int type;
	size_t len;
	const u_char *p;

	if (index < 0) {
		index = lua_gettop(L) + index + 1;
	}

	max = 0;

	lua_pushnil(L); /* stack: table key */
	while (lua_next(L, index) != 0) { /* stack: table key value */
		key = lua_tonumber(L, -2);
		if (key > max) {
			max = key;
		}

		lua_pop(L, 1); /* stack: table key */
	}

	for (i = 1; i <= max; i++) {
		lua_rawgeti(L, index, i); /* stack: table value */
		type = lua_type(L, -1);
		switch (type) {
			case LUA_TNUMBER:
			case LUA_TSTRING:
				p = (u_char *) lua_tolstring(L, -1, &len);
				memcpy(dst, p, len);
				dst += len;
				break;

			case LUA_TNIL:
				*dst++ = 'n';
				*dst++ = 'i';
				*dst++ = 'l';
				break;

			case LUA_TBOOLEAN:
				if (lua_toboolean(L, -1)) {
					*dst++ = 't';
					*dst++ = 'r';
					*dst++ = 'u';
					*dst++ = 'e';

				} else {
					*dst++ = 'f';
					*dst++ = 'a';
					*dst++ = 'l';
					*dst++ = 's';
					*dst++ = 'e';
				}

				break;

			case LUA_TTABLE:
				dst = lua_copy_str_in_table(L, -1, dst);
				break;

			case LUA_TLIGHTUSERDATA:
				*dst++ = 'n';
				*dst++ = 'u';
				*dst++ = 'l';
				*dst++ = 'l';
				break;

			default:
				luaL_error(L, "impossible to reach here");
				return NULL;
		}

		lua_pop(L, 1); /* stack: table */
	}

	return dst;
}

#define base64_encoded_length(len) (((len + 2) / 3) * 4)
#define base64_decoded_length(len) (((len + 3) / 4) * 3)
static char basis64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
static int base64encode(unsigned char *dst, const unsigned char *src, int len){
	unsigned char *d;
	const unsigned char *s;
	
	s = src;
	d = dst;

	while (len > 2) {
		*d++ = basis64[(s[0] >> 2) & 0x3f];
		*d++ = basis64[((s[0] & 3) << 4) | (s[1] >> 4)];
		*d++ = basis64[((s[1] & 0x0f) << 2) | (s[2] >> 6)];
		*d++ = basis64[s[2] & 0x3f];

		s += 3;
		len -= 3;
	}

	if (len) {
		*d++ = basis64[(s[0] >> 2) & 0x3f];

		if (len == 1) {
			*d++ = basis64[(s[0] & 3) << 4];
			*d++ = '=';

		} else {
			*d++ = basis64[((s[0] & 3) << 4) | (s[1] >> 4)];
			*d++ = basis64[(s[1] & 0x0f) << 2];
		}

		*d++ = '=';
	}

	return d - dst;
}

static int base64_decode_internal(unsigned char *dst, const unsigned char *src, size_t slen, const unsigned char *basis){
	size_t len;
	unsigned char *d;
	const unsigned char *s;

	for (len = 0; len < slen; len++) {
		if (src[len] == '=') {
			break;
		}

		if (basis[src[len]] == 77) {
			return 0;
		}
	}

	if (len % 4 == 1) {
		return 0;
	}

	s = src;
	d = dst;

	while (len > 3) {
		*d++ = (char) (basis[s[0]] << 2 | basis[s[1]] >> 4);
		*d++ = (char) (basis[s[1]] << 4 | basis[s[2]] >> 2);
		*d++ = (char) (basis[s[2]] << 6 | basis[s[3]]);

		s += 4;
		len -= 4;
	}

	if (len > 1) {
		*d++ = (char) (basis[s[0]] << 2 | basis[s[1]] >> 4);
	}

	if (len > 2) {
		*d++ = (char) (basis[s[1]] << 4 | basis[s[2]] >> 2);
	}

	return d - dst;
}

static int base64decode(unsigned char *dst, const unsigned char *src, size_t slen){
	static unsigned char basis64[] = {
		77, 77, 77, 77, 77, 77, 77, 77, 77, 77, 77, 77, 77, 77, 77, 77,
		77, 77, 77, 77, 77, 77, 77, 77, 77, 77, 77, 77, 77, 77, 77, 77,
		77, 77, 77, 77, 77, 77, 77, 77, 77, 77, 77, 62, 77, 77, 77, 63,
		52, 53, 54, 55, 56, 57, 58, 59, 60, 61, 77, 77, 77, 77, 77, 77,
		77, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14,
		15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 77, 77, 77, 77, 77,
		77, 26, 27, 28, 29, 30, 31, 32, 33, 34, 35, 36, 37, 38, 39, 40,
		41, 42, 43, 44, 45, 46, 47, 48, 49, 50, 51, 77, 77, 77, 77, 77,

		77, 77, 77, 77, 77, 77, 77, 77, 77, 77, 77, 77, 77, 77, 77, 77,
		77, 77, 77, 77, 77, 77, 77, 77, 77, 77, 77, 77, 77, 77, 77, 77,
		77, 77, 77, 77, 77, 77, 77, 77, 77, 77, 77, 77, 77, 77, 77, 77,
		77, 77, 77, 77, 77, 77, 77, 77, 77, 77, 77, 77, 77, 77, 77, 77,
		77, 77, 77, 77, 77, 77, 77, 77, 77, 77, 77, 77, 77, 77, 77, 77,
		77, 77, 77, 77, 77, 77, 77, 77, 77, 77, 77, 77, 77, 77, 77, 77,
		77, 77, 77, 77, 77, 77, 77, 77, 77, 77, 77, 77, 77, 77, 77, 77,
		77, 77, 77, 77, 77, 77, 77, 77, 77, 77, 77, 77, 77, 77, 77, 77
	};

	return base64_decode_internal(dst, src, slen, basis64);
}

static int base64decode_url(unsigned char *dst, const unsigned char *src, size_t slen){
	static char basis64[] = {
		77, 77, 77, 77, 77, 77, 77, 77, 77, 77, 77, 77, 77, 77, 77, 77,
		77, 77, 77, 77, 77, 77, 77, 77, 77, 77, 77, 77, 77, 77, 77, 77,
		77, 77, 77, 77, 77, 77, 77, 77, 77, 77, 77, 77, 77, 62, 77, 77,
		52, 53, 54, 55, 56, 57, 58, 59, 60, 61, 77, 77, 77, 77, 77, 77,
		77, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14,
		15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 77, 77, 77, 77, 63,
		77, 26, 27, 28, 29, 30, 31, 32, 33, 34, 35, 36, 37, 38, 39, 40,
		41, 42, 43, 44, 45, 46, 47, 48, 49, 50, 51, 77, 77, 77, 77, 77,

		77, 77, 77, 77, 77, 77, 77, 77, 77, 77, 77, 77, 77, 77, 77, 77,
		77, 77, 77, 77, 77, 77, 77, 77, 77, 77, 77, 77, 77, 77, 77, 77,
		77, 77, 77, 77, 77, 77, 77, 77, 77, 77, 77, 77, 77, 77, 77, 77,
		77, 77, 77, 77, 77, 77, 77, 77, 77, 77, 77, 77, 77, 77, 77, 77,
		77, 77, 77, 77, 77, 77, 77, 77, 77, 77, 77, 77, 77, 77, 77, 77,
		77, 77, 77, 77, 77, 77, 77, 77, 77, 77, 77, 77, 77, 77, 77, 77,
		77, 77, 77, 77, 77, 77, 77, 77, 77, 77, 77, 77, 77, 77, 77, 77,
		77, 77, 77, 77, 77, 77, 77, 77, 77, 77, 77, 77, 77, 77, 77, 77
	};

	return base64_decode_internal(dst, src, slen, basis64);
}

char sha_buf[SHA_DIGEST_LENGTH];
int lua_f_sha1bin(lua_State *L){
	const char *src = NULL;
	size_t slen = 0;
	if (lua_isnil(L, 1)) {
		src = "";
	} else {
		src = luaL_checklstring(L, 1, &slen);
	}
	
	SHA_CTX sha;
	SHA1_Init(&sha);
	SHA1_Update(&sha, src, slen);
	SHA1_Final(sha_buf, &sha);
	
	lua_pushlstring(L, (char *) sha_buf, sizeof(sha_buf));
	return 1;
}

int lua_f_base64encode(lua_State *L){
	const char *src = NULL;
	size_t slen = 0;
	if (lua_isnil(L, 1)) {
		src = "";
	} else {
		src = luaL_checklstring(L, 1, &slen);
	}

	char *end = large_malloc(base64_encoded_length(slen));
	int nlen = base64encode(end, src, slen);
	lua_pushlstring(L, end, nlen);
	free(end);
	return 1;
}

int lua_f_base64decode(lua_State *L){
	const char *src = NULL;
	size_t slen = 0;
	if (lua_isnil(L, 1)) {
		src = "";
	} else {
		src = luaL_checklstring(L, 1, &slen);
	}

	char *end = large_malloc(base64_decoded_length(slen));
	int nlen = base64decode(end, src, slen);
	lua_pushlstring(L, end, nlen);
	free(end);
	return 1;
}

#define ESCAPE_URI 0
#define ESCAPE_ARGS 1
#define ESCAPE_URI_COMPONENT 2
#define ESCAPE_HTML 3
#define ESCAPE_REFRESH 4
#define ESCAPE_MEMCACHED 5
#define ESCAPE_MAIL_AUTH 6
#define UNESCAPE_URI 1
#define UNESCAPE_REDIRECT 2
#define UNESCAPE_URI_COMPONENT 0
uintptr_t ngx_http_lua_escape_uri(u_char *dst, u_char *src, size_t size, unsigned int type){
	unsigned int n;
	uint32_t *escape;
	static u_char hex[] = "0123456789abcdef";

					/* " ", "#", "%", "?", %00-%1F, %7F-%FF */

	static uint32_t uri[] = {
		0xffffffff, /* 1111 1111 1111 1111 1111 1111 1111 1111 */

					/* ?>=< ;:98 7654 3210 /.-, +*)( '&%$ #"! */
		0xfc00886d, /* 1111 1100 0000 0000 1000 1000 0110 1101 */

					/* _^]\ [ZYX WVUT SRQP ONML KJIH GFED CBA@ */
		0x78000000, /* 0111 1000 0000 0000 0000 0000 0000 0000 */

					/* ~}| {zyx wvut srqp onml kjih gfed cba` */
		0xa8000000, /* 1010 1000 0000 0000 0000 0000 0000 0000 */

		0xffffffff, /* 1111 1111 1111 1111 1111 1111 1111 1111 */
		0xffffffff, /* 1111 1111 1111 1111 1111 1111 1111 1111 */
		0xffffffff, /* 1111 1111 1111 1111 1111 1111 1111 1111 */
		0xffffffff /* 1111 1111 1111 1111 1111 1111 1111 1111 */
	};

					/* " ", "#", "%", "+", "?", %00-%1F, %7F-%FF */

	static uint32_t args[] = {
		0xffffffff, /* 1111 1111 1111 1111 1111 1111 1111 1111 */

					/* ?>=< ;:98 7654 3210 /.-, +*)( '&%$ #"! */
		0x80000829, /* 1000 0000 0000 0000 0000 1000 0010 1001 */

					/* _^]\ [ZYX WVUT SRQP ONML KJIH GFED CBA@ */
		0x00000000, /* 0000 0000 0000 0000 0000 0000 0000 0000 */

					/* ~}| {zyx wvut srqp onml kjih gfed cba` */
		0x80000000, /* 1000 0000 0000 0000 0000 0000 0000 0000 */

		0xffffffff, /* 1111 1111 1111 1111 1111 1111 1111 1111 */
		0xffffffff, /* 1111 1111 1111 1111 1111 1111 1111 1111 */
		0xffffffff, /* 1111 1111 1111 1111 1111 1111 1111 1111 */
		0xffffffff /* 1111 1111 1111 1111 1111 1111 1111 1111 */
	};

					/* " ", "#", """, "%", "'", %00-%1F, %7F-%FF */

	static uint32_t html[] = {
		0xffffffff, /* 1111 1111 1111 1111 1111 1111 1111 1111 */

					/* ?>=< ;:98 7654 3210 /.-, +*)( '&%$ #"! */
		0x000000ad, /* 0000 0000 0000 0000 0000 0000 1010 1101 */

					/* _^]\ [ZYX WVUT SRQP ONML KJIH GFED CBA@ */
		0x00000000, /* 0000 0000 0000 0000 0000 0000 0000 0000 */

					/* ~}| {zyx wvut srqp onml kjih gfed cba` */
		0x80000000, /* 1000 0000 0000 0000 0000 0000 0000 0000 */

		0xffffffff, /* 1111 1111 1111 1111 1111 1111 1111 1111 */
		0xffffffff, /* 1111 1111 1111 1111 1111 1111 1111 1111 */
		0xffffffff, /* 1111 1111 1111 1111 1111 1111 1111 1111 */
		0xffffffff /* 1111 1111 1111 1111 1111 1111 1111 1111 */
	};

					/* " ", """, "%", "'", %00-%1F, %7F-%FF */

	static uint32_t refresh[] = {
		0xffffffff, /* 1111 1111 1111 1111 1111 1111 1111 1111 */

					/* ?>=< ;:98 7654 3210 /.-, +*)( '&%$ #"! */
		0x00000085, /* 0000 0000 0000 0000 0000 0000 1000 0101 */

					/* _^]\ [ZYX WVUT SRQP ONML KJIH GFED CBA@ */
		0x00000000, /* 0000 0000 0000 0000 0000 0000 0000 0000 */

					/* ~}| {zyx wvut srqp onml kjih gfed cba` */
		0x80000000, /* 1000 0000 0000 0000 0000 0000 0000 0000 */

		0xffffffff, /* 1111 1111 1111 1111 1111 1111 1111 1111 */
		0xffffffff, /* 1111 1111 1111 1111 1111 1111 1111 1111 */
		0xffffffff, /* 1111 1111 1111 1111 1111 1111 1111 1111 */
		0xffffffff /* 1111 1111 1111 1111 1111 1111 1111 1111 */
	};

					/* " ", "%", %00-%1F */

	static uint32_t memcached[] = {
		0xffffffff, /* 1111 1111 1111 1111 1111 1111 1111 1111 */

					/* ?>=< ;:98 7654 3210 /.-, +*)( '&%$ #"! */
		0x00000021, /* 0000 0000 0000 0000 0000 0000 0010 0001 */

					/* _^]\ [ZYX WVUT SRQP ONML KJIH GFED CBA@ */
		0x00000000, /* 0000 0000 0000 0000 0000 0000 0000 0000 */

					/* ~}| {zyx wvut srqp onml kjih gfed cba` */
		0x00000000, /* 0000 0000 0000 0000 0000 0000 0000 0000 */

		0x00000000, /* 0000 0000 0000 0000 0000 0000 0000 0000 */
		0x00000000, /* 0000 0000 0000 0000 0000 0000 0000 0000 */
		0x00000000, /* 0000 0000 0000 0000 0000 0000 0000 0000 */
		0x00000000, /* 0000 0000 0000 0000 0000 0000 0000 0000 */
	};

					/* mail_auth is the same as memcached */

	static uint32_t *map[] =
		{ uri, args, html, refresh, memcached, memcached };


	escape = map[type];

	if (dst == NULL) {

		/* find the number of the characters to be escaped */

		n = 0;

		while (size) {
			if (escape[*src >> 5] & (1 << (*src & 0x1f))) {
				n++;
			}
			src++;
			size--;
		}

		return (uintptr_t) n;
	}

	while (size) {
		if (escape[*src >> 5] & (1 << (*src & 0x1f))) {
			*dst++ = '%';
			*dst++ = hex[*src >> 4];
			*dst++ = hex[*src & 0xf];
			src++;

		} else {
			*dst++ = *src++;
		}
		size--;
	}

	return (uintptr_t) dst;
}


/* XXX we also decode '+' to ' ' */
void ngx_http_lua_unescape_uri(u_char **dst, u_char **src, size_t size, unsigned int type){
	u_char *d, *s, ch, c, decoded;
	enum {
		sw_usual = 0,
		sw_quoted,
		sw_quoted_second
	} state;

	d = *dst;
	s = *src;

	state = 0;
	decoded = 0;

	while (size--) {

		ch = *s++;

		switch (state) {
		case sw_usual:
			if (ch == '?'
				&& (type & (UNESCAPE_URI|UNESCAPE_REDIRECT)))
			{
				*d++ = ch;
				goto done;
			}

			if (ch == '%') {
				state = sw_quoted;
				break;
			}

			if (ch == '+') {
				*d++ = ' ';
				break;
			}

			*d++ = ch;
			break;

		case sw_quoted:

			if (ch >= '0' && ch <= '9') {
				decoded = (u_char) (ch - '0');
				state = sw_quoted_second;
				break;
			}

			c = (u_char) (ch | 0x20);
			if (c >= 'a' && c <= 'f') {
				decoded = (u_char) (c - 'a' + 10);
				state = sw_quoted_second;
				break;
			}

			/* the invalid quoted character */

			state = sw_usual;

			*d++ = ch;

			break;

		case sw_quoted_second:

			state = sw_usual;

			if (ch >= '0' && ch <= '9') {
				ch = (u_char) ((decoded << 4) + ch - '0');

				if (type & UNESCAPE_REDIRECT) {
					if (ch > '%' && ch < 0x7f) {
						*d++ = ch;
						break;
					}

					*d++ = '%'; *d++ = *(s - 2); *d++ = *(s - 1);
					break;
				}

				*d++ = ch;

				break;
			}

			c = (u_char) (ch | 0x20);
			if (c >= 'a' && c <= 'f') {
				ch = (u_char) ((decoded << 4) + c - 'a' + 10);

				if (type & UNESCAPE_URI) {
					if (ch == '?') {
						*d++ = ch;
						goto done;
					}

					*d++ = ch;
					break;
				}

				if (type & UNESCAPE_REDIRECT) {
					if (ch == '?') {
						*d++ = ch;
						goto done;
					}

					if (ch > '%' && ch < 0x7f) {
						*d++ = ch;
						break;
					}

					*d++ = '%'; *d++ = *(s - 2); *d++ = *(s - 1);
					break;
				}

				*d++ = ch;

				break;
			}

			/* the invalid quoted character */

			break;
		}
	}

done:

	*dst = d;
	*src = s;
}

int lua_f_escape_uri(lua_State *L){
	size_t len, dlen;
	uintptr_t escape;
	u_char *src, *dst;

	if (lua_gettop(L) != 1) {
		return luaL_error(L, "expecting one argument");
	}

	src = (u_char *) luaL_checklstring(L, 1, &len);

	if (len == 0) {
		return 1;
	}

	escape = 2 * ngx_http_lua_escape_uri(NULL, src, len, ESCAPE_URI);

	if (escape) {
		dlen = escape + len;
		dst = lua_newuserdata(L, dlen);
		ngx_http_lua_escape_uri(dst, src, len, ESCAPE_URI);
		lua_pushlstring(L, (char *) dst, dlen);
	}

	return 1;
}

int lua_f_unescape_uri(lua_State *L){
	size_t len, dlen;
	u_char *p;
	u_char *src, *dst;

	if (lua_gettop(L) != 1) {
		return luaL_error(L, "expecting one argument");
	}

	src = (u_char *) luaL_checklstring(L, 1, &len);

	/* the unescaped string can only be smaller */
	dlen = len;

	p = lua_newuserdata(L, dlen);

	dst = p;

	ngx_http_lua_unescape_uri(&dst, &src, len, UNESCAPE_URI_COMPONENT);

	lua_pushlstring(L, (char *) p, dst - p);

	return 1;
}
