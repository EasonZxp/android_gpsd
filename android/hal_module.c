#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <pthread.h>
#include <fcntl.h>
#include <sys/epoll.h>
#include <sys/timerfd.h>
#include <math.h>
#include <time.h>

#include <gps.h>

#define  LOG_TAG  "gps_gpsd"
#include <cutils/log.h>
#include <cutils/sockets.h>
#include <hardware/gps.h>

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <strings.h>

#define  GPS_DEBUG  1

#if GPS_DEBUG
#  define  D(...)   ALOGD(__VA_ARGS__)
//#  define  V(...)   ALOGV(__VA_ARGS__)
#  define  V(...)   ((void)0)
#else
#  define  D(...)   ((void)0)
#  define  V(...)   ((void)0)
#endif

#if !defined(MIN)
#  define MIN(x,y) ((x) < (y) ? (x) : (y))
#endif

enum {
	FD_CONTROL = 0,
	FD_WORKER = 1,
};

/* commands sent to the gps thread */
enum {
	CMD_QUIT,
	CMD_START,
	CMD_STOP,
	CMD_CHANGE_INTERVAL,
	_CMD_COUNT
};

enum {
	NMEA_MODE_GNSS,
	NMEA_MODE_GPS,
	NMEA_MODE_BDS,
};

typedef struct {
	int initialized:1;
	int watch_enabled:1;
	struct gps_data_t gps_data;
	int control_fds[2];
	pthread_t worker;
	int epoll_fd;
	int timer_fd;
	GpsCallbacks callbacks;
	struct timespec report_interval;
	char *version;
	GpsStatus status;
	int fix_mode;
	int rnss_mode;
	int last_reported_fix_mode;
	GpsLocation location;
} GpsState;

typedef struct {
	int control_fds[2];
	pthread_t nmea_t;
	int listenfd;
	int connfd;
	int epoll_fd;
} GpsNmea;

static GpsNmea _gps_nmea[1];

static GpsState _gps_state[1];

static void epoll_add(int epoll_fd, int fd, int events)
{
	struct epoll_event ev;

	ev.events = events;
	ev.data.fd = fd;
	if (TEMP_FAILURE_RETRY(epoll_ctl(epoll_fd, EPOLL_CTL_ADD, fd, &ev)) < 0)
		ALOGE("epoll_add() unexpected error: %s", strerror(errno));
}

static void epoll_del(int epoll_fd, int fd)
{
	struct epoll_event ev;

	if (TEMP_FAILURE_RETRY(epoll_ctl(epoll_fd, EPOLL_CTL_DEL, fd, &ev)) < 0)
		ALOGE("epoll_del() unexpected error: %s", strerror(errno));
}

static int write_control_command(GpsState *s, char cmd)
{
	return TEMP_FAILURE_RETRY(write(s->control_fds[FD_CONTROL], &cmd,
									sizeof(cmd)));
}

static int nmea_write_control_command(GpsNmea *nmea, char cmd)
{
	return TEMP_FAILURE_RETRY(write(nmea->control_fds[FD_CONTROL], &cmd,
									sizeof(cmd)));
}

static void setup_timerfd(int timer_fd, const struct timespec *preferred)
{
	if (timer_fd < 0)
		return;

	struct itimerspec its;

	memset(&its, 0, sizeof(its));
	timerfd_gettime(timer_fd, &its);
	if ((preferred->tv_sec == its.it_interval.tv_sec)
		&& (preferred->tv_nsec == its.it_interval.tv_nsec))
		return;

	its.it_value = *preferred;
	its.it_interval = *preferred;
	if (timerfd_settime(timer_fd, 0, &its, NULL) < 0)
		ALOGE("timerfd_settime() unexpected error: %s", strerror(errno));
}

static void disable_timerfd(int timer_fd)
{
	if (timer_fd < 0)
		return;

	struct itimerspec its;

	memset(&its, 0, sizeof(its));
	timerfd_settime(timer_fd, 0, &its, NULL);
}

static void report_status(GpsState *s, GpsStatusValue status)
{
	if (s->status.status == status)
		return;

	s->status.status = status;

	if (!s->callbacks.status_cb)
		return;

	D("status_cb({.status: %d})", s->status.status);
	s->callbacks.status_cb(&s->status);
}

static void report_location(GpsState *s, int timer_triggered)
{
	if (!timer_triggered) {
		// Periodical report triggered by gpsd event.

		if (!(s->location.flags & GPS_LOCATION_HAS_LAT_LONG)
			&& (s->last_reported_fix_mode < MODE_2D)) {
			V("skipped continuous NO_FIX location reports");
			return;
		}

		if (s->report_interval.tv_sec || s->report_interval.tv_nsec) {
			V("location to be reported in next timer event");
			return;
		}
	}

	s->last_reported_fix_mode = s->fix_mode;

	if (!s->callbacks.location_cb || s->fix_mode < MODE_2D)
		return;

	D("location_cb({.lat: %.6f, .lon: %.6f, .flags: %d, "
	  ".speed: %.3f, .bearing: %.1f, "
	  ".accuracy: %d, .timestamp: %"PRId64"}), mode: %d",
	  s->location.latitude, s->location.longitude,
	  s->location.flags, s->location.speed,
	  s->location.bearing, (int) floor(s->location.accuracy + 0.5),
	  s->location.timestamp, s->fix_mode);
	s->callbacks.location_cb(&s->location);
}

static void reconnect_gpsd(GpsState *s)
{
	int connected = 1;
	int ret = 0;
#if 0
	ret = gps_open(NULL, NULL, &s->gps_data);
	if (ret) {
		connected = 0;
		ALOGE("gps open failed : %d\n", ret);
	} else if (gps_stream(&s->gps_data, WATCH_ENABLE, NULL)) {
		connected = 0;
		ALOGE("gps stream failed : %d\n", ret);
		gps_close(&s->gps_data);
	}
#else
	ret = gps_open("localhost", "2947", &s->gps_data);
	if (0 == ret) {
		ALOGI("gps_open (success)\n");
		if(gps_stream(&s->gps_data, WATCH_ENABLE, NULL)){
			connected = 0;
			ALOGE("gps stream failed : %d\n", ret);
			gps_close(&s->gps_data);
		}
	} else {
		ALOGE("gps_open failed, returned: %d\n", ret);
		connected = 0;
	}
#endif
	if (connected) {
		ALOGI("epoll will add gps_fd : %d\n", s->gps_data.gps_fd);
		epoll_add(s->epoll_fd, s->gps_data.gps_fd,
				  EPOLLIN | EPOLLERR | EPOLLHUP);

		s->status.status = GPS_STATUS_NONE;
		s->fix_mode = MODE_NOT_SEEN;
		s->last_reported_fix_mode = MODE_NOT_SEEN;
		s->location.flags = 0;

		if (gps_send(&s->gps_data, "?DEVICES;\n")) {
			ALOGE("Failed to query devices list. Assume off.");
			report_status(s, GPS_STATUS_ENGINE_OFF);
		}

		setup_timerfd(s->timer_fd, &s->report_interval);
		return;
	}

	ALOGE("!failed to connect gpsd server");

	// Setup reconnect timer
	struct itimerspec its;

	its.it_interval.tv_sec = its.it_value.tv_sec = 3;
	its.it_interval.tv_nsec = its.it_value.tv_nsec = 0;
	if (timerfd_settime(s->timer_fd, 0, &its, NULL) < 0)
		ALOGE("failed to setup reconnect timer. Stopped forever.");
}

static int power_flag = 0;
#define BD_GPS_CONTORL_PATH "/sys/class/hd8020_bdgps_power/hd8020_bdgps/ctl_hd8020"
int power_ctrl(int enable)
{
	int fd = 0;
	char c_enable = '1';
	char c_disable = '0';

	fd = open(BD_GPS_CONTORL_PATH, O_WRONLY);
	if(fd < 0){
		D("open %s failed\n", BD_GPS_CONTORL_PATH);
		return -1;
	}

	if(!power_flag && enable)
	{
		if(write(fd, &c_enable, 1) < 0)
		{
			D("write %s failed\n", BD_GPS_CONTORL_PATH);
			close(fd);
			return -2;
		}
		power_flag = 1;
		D("power on.\n");
	}else if(power_flag && !enable){
		if(write(fd, &c_disable, 1) < 0)
		{
			D("write %s failed\n", BD_GPS_CONTORL_PATH);
			close(fd);
			return -2;
		}
		D("power off.\n");
		power_flag = 0;
	}
	close(fd);
	return 0;
}

static int set_rnss_mode(GpsState *s)
{
	int ret = 0;

	if(s->rnss_mode == NMEA_MODE_GPS){
		ret = gps_send(&s->gps_data, "!NMEA_MODE=GPS");
	}else if(s->rnss_mode == NMEA_MODE_BDS){
		ret = gps_send(&s->gps_data, "!NMEA_MODE=BDS");
	}
	else{
		ret = gps_send(&s->gps_data, "!NMEA_MODE=GNSS");
	}

	return ret;
}

static void handle_control_start(GpsState *s)
{
	int ret = 0;

	if (s->watch_enabled)
		return;

	s->watch_enabled = 1;
	reconnect_gpsd(s);
	power_ctrl(1);

#if 1
	sleep(1);//need 1 second delay to bds module bootup
	ret = set_rnss_mode(s);
	if(ret)
		ALOGE("gps send nmea mode error\n");
#endif
}

static void handle_control_stop(GpsState *s)
{
	if (!s->watch_enabled)
		return;

	s->watch_enabled = 0;

	if (s->gps_data.gps_fd >= 0) {
		epoll_del(s->epoll_fd, s->gps_data.gps_fd);

		gps_stream(&s->gps_data, WATCH_DISABLE, NULL);
		gps_close(&s->gps_data);
	}

	disable_timerfd(s->timer_fd);
	power_ctrl(0);
	s->rnss_mode = NMEA_MODE_GNSS;
}

static int handle_control(GpsState *s)
{
	char cmd = _CMD_COUNT;

	if (TEMP_FAILURE_RETRY(read(s->control_fds[FD_WORKER], &cmd,
								sizeof(cmd))) < 0) {
		ALOGE("read control fd unexpected error: %s", strerror(errno));
		return -1;
	}

	V("gps thread control command: %d", cmd);

	switch (cmd) {
	case CMD_QUIT:
		return -1;

	case CMD_START:
		handle_control_start(s);
		break;

	case CMD_STOP:
		handle_control_stop(s);
		break;

	case CMD_CHANGE_INTERVAL:
		if (s->status.status == GPS_STATUS_SESSION_BEGIN)
			setup_timerfd(s->timer_fd, &s->report_interval);

		break;

	default:
		ALOGE("unknown control command: %d", cmd);
		break;
	}

	return 0;
}

static int nmea_handle_control(GpsNmea *nmea)
{
	char cmd = _CMD_COUNT;

	if (TEMP_FAILURE_RETRY(read(nmea->control_fds[FD_WORKER], &cmd,
								sizeof(cmd))) < 0) {
		ALOGE("read control fd unexpected error: %s", strerror(errno));
		return -1;
	}

	V("nmea thread control command: %d", cmd);

	switch (cmd) {
	case CMD_QUIT:
		return -1;

	case CMD_START:
		break;

	case CMD_STOP:
		break;

	case CMD_CHANGE_INTERVAL:
		break;

	default:
		ALOGE("nmea unknown control command: %d", cmd);
		break;
	}

	return 0;
}

static void handle_gpsd_version(GpsState *s, struct gps_data_t *gps_data)
{
	int needed;

#define VERSION_PRINTF_ARGS \
	"gpsd release %s rev %s", \
	gps_data->version.release, gps_data->version.rev
	needed = snprintf(NULL, 0, VERSION_PRINTF_ARGS);
	if (!s->version || ((int)strlen(s->version) < needed)) {
		if (s->version)
			free(s->version);
		s->version = (char*) calloc(needed + 1, 1);
	}
	snprintf(s->version, needed + 1, VERSION_PRINTF_ARGS);
#undef VERSION_PRINTF_ARGS

	ALOGI("%s", s->version);
}

static void handle_gpsd_devicelist(GpsState *s, struct gps_data_t *gps_data)
{
	int activated = 0;

	for (int i = 0; i < gps_data->devices.ndevices; ++i) {
		double timestamp = (double)(gps_data->devices.list[i].activated);
		if (!isnan(timestamp) && (floor(timestamp) > 0)) {
			activated = 1;
			break;
		}
	}

	if (!activated)
		report_status(s, GPS_STATUS_ENGINE_OFF);
	else if ((s->status.status == GPS_STATUS_ENGINE_OFF)
			 || (s->status.status == GPS_STATUS_NONE))
		report_status(s, GPS_STATUS_ENGINE_ON);
}

static void handle_gpsd_satellite_gnss(GpsState *s,
									   struct gps_data_t *gps_data)
{
	GnssSvStatus statuses;
	int used = 0;

	statuses.size = sizeof(statuses);
	statuses.num_svs =
		MIN(GNSS_MAX_SVS, gps_data->satellites_visible);
	for (int i = 0; i < statuses.num_svs; ++i) {
		GnssSvInfo *info = &statuses.gnss_sv_list[i];
		const struct satellite_t *satellite = &gps_data->skyview[i];

		info->size = sizeof(info);

		info->svid = satellite->PRN;
		if (GPS_PRN(satellite->PRN))
			info->constellation = GNSS_CONSTELLATION_GPS;
		else if (GBAS_PRN(satellite->PRN)) {
			info->constellation = GNSS_CONSTELLATION_GLONASS;
			info->svid -= GLONASS_PRN_OFFSET;
		} else if (SBAS_PRN(satellite->PRN))
			info->constellation = GNSS_CONSTELLATION_SBAS;
#define QZSS_PRN(n) (((n) >= 193) && ((n) <= 200))
		else if (QZSS_PRN(satellite->PRN))
			info->constellation = GNSS_CONSTELLATION_QZSS;
#define BEIDOU_PRN(n) (((n) >= 201) && ((n) <= 235))
		else if (BEIDOU_PRN(satellite->PRN)) {
			info->constellation = GNSS_CONSTELLATION_BEIDOU;
			info->svid -= 200;
		} else
			info->constellation = GNSS_CONSTELLATION_UNKNOWN;

		info->c_n0_dbhz = satellite->ss;
		info->elevation = satellite->elevation;
		info->azimuth = satellite->azimuth;
		info->flags = GNSS_SV_FLAGS_NONE;
		//D("==> prn[%d]: %d, cn0:%f, used:%d\n", i, satellite->PRN, satellite->ss, satellite->used);
		if (satellite->used) {
			info->flags |= GNSS_SV_FLAGS_USED_IN_FIX;
			used++;
		}
	}

	D("gnss_sv_status_cb({.num_svs: %d, ...}), used=%d",
	  statuses.num_svs, used);
	s->callbacks.gnss_sv_status_cb(&statuses);
}

static void handle_gpsd_satellite_legacy(GpsState *s,
										 struct gps_data_t *gps_data)
{
	GpsSvStatus statuses;

	statuses.size = sizeof(statuses);
	statuses.num_svs =
		MIN(GPS_MAX_SVS, gps_data->satellites_visible);
	statuses.ephemeris_mask = 0;
	statuses.almanac_mask = 0;
	statuses.used_in_fix_mask = 0;
	for (int i = 0; i < statuses.num_svs; ++i) {
		GpsSvInfo *info = &statuses.sv_list[i];
		const struct satellite_t *satellite = &gps_data->skyview[i];

		info->size = sizeof(info);
		info->prn = satellite->PRN;
		info->snr = satellite->ss;
		info->elevation = satellite->elevation;
		info->azimuth = satellite->azimuth;
		if (satellite->used)
			statuses.used_in_fix_mask |= (0x01U << i);
	}

	D("sv_status_cb({.num_svs: %d, ...})", statuses.num_svs);
	s->callbacks.sv_status_cb(&statuses);
}

static void handle_gpsd_satellite(GpsState *s, struct gps_data_t *gps_data)
{
	report_status(s, GPS_STATUS_SESSION_BEGIN);

	if (s->callbacks.gnss_sv_status_cb)
		handle_gpsd_satellite_gnss(s, gps_data);
	else if (s->callbacks.sv_status_cb)
		handle_gpsd_satellite_legacy(s, gps_data);
}

static void handle_gpsd_status(GpsState *s, struct gps_data_t *gps_data)
{
	report_status(s, GPS_STATUS_SESSION_BEGIN);

	s->fix_mode = gps_data->fix.mode;
	if (s->fix_mode < MODE_2D) {
		V("No fix yet. Ignored.");
		return;
	}

	s->location.flags = 0;
	if ((gps_data->set & LATLON_SET) && (gps_data->set & TIME_SET)) {
		s->location.latitude = gps_data->fix.latitude;
		s->location.longitude = gps_data->fix.longitude;
		s->location.timestamp = floor(gps_data->fix.time.tv_sec * 1000 + gps_data->fix.time.tv_nsec / 1000000);
		//D("fix.time.sec:%ld, nsec:%ld, timestamp:%lld\n", gps_data->fix.time.tv_sec, gps_data->fix.time.tv_nsec, (long long)s->location.timestamp);
		s->location.flags |= GPS_LOCATION_HAS_LAT_LONG;
	}
	if (gps_data->set & ALTITUDE_SET) {
#if 0
		s->location.altitude = gps_data->fix.altitude;
#else
		s->location.altitude = gps_data->fix.altHAE;
#endif
		s->location.flags |= GPS_LOCATION_HAS_ALTITUDE;
	}
	if (gps_data->set & SPEED_SET) {
		s->location.speed = gps_data->fix.speed;
		s->location.flags |= GPS_LOCATION_HAS_SPEED;
	}
	if (gps_data->set & TRACK_SET) {
		s->location.bearing = gps_data->fix.track;
		s->location.flags |= GPS_LOCATION_HAS_BEARING;
	}
#if 0
	if (gps_data->set & (HERR_SET | VERR_SET)) {
		double err = 0.0;
		if (gps_data->set & HERR_SET)
			err = (gps_data->fix.epx > gps_data->fix.epy) ?
				gps_data->fix.epx : gps_data->fix.epy;
		if ((gps_data->set & VERR_SET) && (gps_data->fix.epv > err))
			err = gps_data->fix.epv;

		s->location.accuracy = err;
		s->location.flags |= GPS_LOCATION_HAS_ACCURACY;
	}
#else
	s->location.accuracy = gps_data->dop.hdop;
	s->location.flags |= GPS_LOCATION_HAS_ACCURACY;
#endif
	report_location(s, 0);
}

static int handle_gpsd(GpsState *s)
{
	struct gps_data_t *gps_data = &s->gps_data;
	if (gps_read(gps_data, NULL, 0) == -1) {
		ALOGE("error while reading from gps daemon socket: %s:",
			  strerror(errno));
		return -1;
	}

	if (gps_data->set & VERSION_SET)
		handle_gpsd_version(s, gps_data);

	if (gps_data->set & DEVICELIST_SET)
		handle_gpsd_devicelist(s, gps_data);

	if (gps_data->set & SATELLITE_SET)
		handle_gpsd_satellite(s, gps_data);

	if (gps_data->set & STATUS_SET)
		handle_gpsd_status(s, gps_data);

	return 0;
}

static void handle_timer(GpsState *s)
{
	// discard content for next run.
	uint64_t count;
	read(s->timer_fd, &count, sizeof(count));

	if (s->gps_data.gps_fd >= 0)
		report_location(s, 1);
	else if (s->watch_enabled)
		reconnect_gpsd(s);
}

static int worker_loop(GpsState *s)
{
	struct epoll_event events[3];
	int ne, nevents;
	int ret = 0;

	nevents = epoll_wait(s->epoll_fd, events,
						 (sizeof(events) / sizeof(events[0])), -1);
	if (nevents < 0) {
		if (errno != EINTR)
			ALOGE("epoll_wait() unexpected error: %s", strerror(errno));
		return ret;
	}

	if (s->callbacks.acquire_wakelock_cb)
		s->callbacks.acquire_wakelock_cb();

	for (ne = 0; ne < nevents; ne++) {
		struct epoll_event *event = &events[ne];
		int fd = events[ne].data.fd;

		if (fd == s->control_fds[FD_WORKER]) {
			V("events %d for worker control fd", event->events);
			if (handle_control(s) < 0) {
				ret = -1;
				break;
			}
		} else if (fd == s->gps_data.gps_fd) {
			V("events %d for gpsd socket", event->events);
			if ((event->events & (EPOLLERR | EPOLLHUP))
				|| (handle_gpsd(s) < 0)) {
				ALOGE("gpsd socket error. reconnecting ...");

				epoll_del(s->epoll_fd, s->gps_data.gps_fd);
				gps_close(&s->gps_data);

				reconnect_gpsd(s);
			}
		} else if (fd == s->timer_fd) {
			V("events %d for timer fd", event->events);
			handle_timer(s);
		}
		else
			ALOGE("epoll_wait() returned unkown fd %d ?", fd);
	} /* nevents loop */

	if (s->callbacks.release_wakelock_cb)
		s->callbacks.release_wakelock_cb();

	return ret;
}

static void worker_thread(void *thread_data)
{
	GpsState *s = (GpsState*)thread_data;
	uint32_t capabilities = 0;

	ALOGI("work gps thread running");

	// register file descriptors for polling
	s->epoll_fd = epoll_create(3);
	ALOGD("worker  epoll add control_fds:%d\n", s->control_fds[FD_WORKER]);
	epoll_add(s->epoll_fd, s->control_fds[FD_WORKER], EPOLLIN);

	s->timer_fd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
	if (s->timer_fd < 0)
		ALOGE("timerfd_create() unexpected error: %s", strerror(errno));
	else {
		epoll_add(s->epoll_fd, s->timer_fd, EPOLLIN);

		capabilities |= GPS_CAPABILITY_SCHEDULING;
	}

	if (s->callbacks.set_capabilities_cb)
		s->callbacks.set_capabilities_cb(capabilities);

	// now loop
	while (worker_loop(s) == 0);

	s->watch_enabled = 0;

	if (s->version) {
		free(s->version);
		s->version = NULL;
	}
#if 0
	if (s->gps_data.gps_fd >= 0)
		gps_close(&s->gps_data);
#endif
	if (s->timer_fd >= 0) {
		close(s->timer_fd);
		s->timer_fd = -1;
	}

	close(s->epoll_fd);
	s->epoll_fd = -1;

	ALOGI("gps thread quit");
}

#define MAXBUF 1024

/*
 * 设置句柄为非阻塞方式
 */
int setnonblocking(int sockfd)
{
	if (fcntl(sockfd, F_SETFL, fcntl(sockfd, F_GETFD, 0)|O_NONBLOCK) == -1)
	{
		return -1;
	}
	return 0;
}

int create_nmea_socket()
{
	int listenfd;
	struct sockaddr_in my_addr;
	char *IP = "127.0.0.1";
	short PORT = 2948;

	ALOGI("nmea gps thread running");

	// 开启 socket 监听
	if ((listenfd = socket(PF_INET, SOCK_STREAM, 0)) == -1)
	{
		ALOGE("socket create failed\n");
		return -1;
	} else
		ALOGD("socket create successfully.\n");

	// 把socket设置为非阻塞方式
	setnonblocking(listenfd);

	bzero(&my_addr, sizeof(my_addr));
	my_addr.sin_family = PF_INET;
	my_addr.sin_port = htons(PORT);
	my_addr.sin_addr.s_addr = inet_addr(IP);

	if (bind(listenfd, (struct sockaddr *) &my_addr, sizeof(struct sockaddr)) == -1)
	{
		ALOGE("bind failed\n");
		goto Exit;
	} else
		ALOGD("IP addr and port bind successfully.\n");

	if (listen(listenfd, 2) == -1)
	{
		ALOGE("listen failed\n");
		goto Exit;
	} else
		ALOGD("socket listen successfully.\n");
	return listenfd;

Exit:
	close(listenfd);
	listenfd = -1;
	return listenfd;
}

static int nmea_loop(GpsNmea *nmea)
{
	struct epoll_event events[3];
	int ne, nevents;
	int ret = 0;
	socklen_t len;
	struct sockaddr_in their_addr;
	char buf[MAXBUF + 1];
	GpsState *s = _gps_state;

	nevents = epoll_wait(nmea->epoll_fd, events,
						 (sizeof(events) / sizeof(events[0])), -1);
	if (nevents < 0) {
		if (errno != EINTR)
			ALOGE("epoll_wait() unexpected error: %s", strerror(errno));
		return ret;
	}

	for (ne = 0; ne < nevents; ne++) {
		struct epoll_event *event = &events[ne];
		int fd = events[ne].data.fd;

		if (fd == nmea->control_fds[FD_WORKER]) {
			D("events %d for nmea control fd:%d", event->events, fd);

			if (nmea_handle_control(nmea) < 0) {
				ret = -1;
				break;
			}
		} else if (fd == nmea->listenfd) {
			D("events %d for listenfd, fd:%d", event->events, fd);
			if ((event->events & (EPOLLERR | EPOLLHUP))) {
				ALOGE("listenfd error...");
			}

			len = sizeof(struct sockaddr);
			nmea->connfd = accept(nmea->listenfd, (struct sockaddr *) &their_addr, &len);
			if (nmea->connfd < 0)
			{
				ALOGE("accept failed\n");
				continue;
			}
			else
				ALOGD("got a connect: %s:%d， connfd:%d\n", inet_ntoa(their_addr.sin_addr), ntohs(their_addr.sin_port), nmea->connfd);

			setnonblocking(nmea->connfd);
			ALOGD("nmea epoll add connfd:%d\n", nmea->connfd);
			epoll_add(nmea->epoll_fd, nmea->connfd, EPOLLIN | EPOLLERR | EPOLLHUP | EPOLLRDHUP);
		} else if (fd == nmea->connfd) {
			int len;

			if ((fd < 0) || (event->events & (EPOLLERR | EPOLLHUP))) {
				ALOGE("connfd error...");
				continue;
			}
			if ((event->events &  EPOLLRDHUP)) {//client socket closed
				ALOGE("epoll rdhup");
				epoll_del(nmea->epoll_fd, nmea->connfd);
				close(nmea->connfd);
				nmea->connfd = -1;
				continue;
			}
			V("NMEA EPOLLIN fd:%d\n", nmea->connfd);

			bzero(buf, MAXBUF + 1);

			len = recv(nmea->connfd, buf, MAXBUF, 0);
			if (len > 0){
				//TBD: here add check_rnss_mode?
				if(s->callbacks.nmea_cb)
				{
					//ALOGD("ready to send : %s", buf);
					s->callbacks.nmea_cb(s->location.timestamp, buf, len);
				}
			}
			else
			{
				ALOGD("recv error: %d, err: '%s', len:%d\n", errno, strerror(errno), len);
				epoll_del(nmea->epoll_fd, nmea->connfd);
				nmea->connfd = -1;
				continue;
			}
		}
		else
			ALOGE("epoll_wait() returned unkown fd %d ?", fd);
	} /* nevents loop */

	return ret;
}

static void nmea_thread(void *thread_data)
{
	GpsNmea *nmea = (GpsNmea*)thread_data;

	ALOGI("nmea gps thread running");

	// register file descriptors for polling
	nmea->epoll_fd = epoll_create(3);
	D("nmea epoll add control_fds:%d\n", nmea->control_fds[FD_WORKER]);
	epoll_add(nmea->epoll_fd, nmea->control_fds[FD_WORKER], EPOLLIN);

	nmea->listenfd = create_nmea_socket();
	D("nmea epoll add listenfd:%d\n", nmea->listenfd);
	epoll_add(nmea->epoll_fd, nmea->listenfd, EPOLLIN);
	// now loop
	while (nmea_loop(nmea) == 0);

	close(nmea->epoll_fd);
	nmea->epoll_fd = -1;

	close(nmea->listenfd);
	nmea->listenfd = -1;

	ALOGI("nmea thread quit");
}

static int gps_iface_init(GpsCallbacks *callbacks)
{
	GpsState *s = _gps_state;
	GpsNmea *nmea = _gps_nmea;

	if (s->initialized)
		return 0;

	memset(s, 0, sizeof(*s));
	memset(nmea, 0, sizeof(*nmea));

	// System GpsCallbacks might be smaller/larger than what we have.
	memcpy(&s->callbacks, callbacks,
		   MIN(callbacks->size, sizeof(s->callbacks)));

	// Initialize fields shouldn't change between sessions.
	s->status.size = sizeof(s->status);
	s->location.size = sizeof(s->location);

	if (socketpair(AF_LOCAL, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0,
				   s->control_fds) < 0) {
		ALOGE("failed to create control sockets");
		return -1;
	}
	s->worker = callbacks->create_thread_cb("gps_worker_thread",
											worker_thread, s);
	if (!s->worker) {
		ALOGE("could not create gps thread: %s", strerror(errno));

		close(s->control_fds[FD_CONTROL]);
		s->control_fds[FD_CONTROL] = -1;
		close(s->control_fds[FD_WORKER]);
		s->control_fds[FD_WORKER] = -1;
		return -1;
	}

	if (socketpair(AF_LOCAL, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0,
				   nmea->control_fds) < 0) {
		ALOGE("failed to create nmea control sockets");
		return -1;
	}

	nmea->nmea_t = callbacks->create_thread_cb("gps_nmea_thread",
											   nmea_thread, nmea);
	if (!nmea->nmea_t) {
		ALOGE("could not create nmea thread: %s", strerror(errno));
		close(nmea->control_fds[FD_CONTROL]);
		nmea->control_fds[FD_CONTROL] = -1;
		close(nmea->control_fds[FD_WORKER]);
		nmea->control_fds[FD_WORKER] = -1;
		return -1;
	}

	s->initialized = 1;
	V("gps state initialized");

	return 0;
}

static void gps_iface_cleanup()
{
	GpsState *s = _gps_state;
	GpsNmea *nmea = _gps_nmea;

	if (!s->initialized)
		return;

	// tell the thread to quit, and wait for it
	void *dummy;
	write_control_command(s, CMD_QUIT);
	nmea_write_control_command(nmea, CMD_QUIT);
	pthread_join(nmea->nmea_t, &dummy);
	pthread_join(s->worker, &dummy);

	// close the control socket pair
	close(s->control_fds[FD_CONTROL]);
	s->control_fds[FD_CONTROL] = -1;
	close(s->control_fds[FD_WORKER]);
	s->control_fds[FD_WORKER] = -1;

	close(nmea->control_fds[FD_CONTROL]);
	nmea->control_fds[FD_CONTROL] = -1;
	close(nmea->control_fds[FD_WORKER]);
	nmea->control_fds[FD_WORKER] = -1;

	s->initialized = 0;
}


static int gps_iface_start()
{
	GpsState *s = _gps_state;
	GpsNmea *nmea = _gps_nmea;
	int ret = 0;

	if (!s->initialized) {
		ALOGE("%s: called with uninitialized state !!", __FUNCTION__);
		return -1;
	}

	ret = nmea_write_control_command(nmea, CMD_START);
	D("%s, nmea write cmd start :%d\n", __func__, ret);

	ret = write_control_command(s, CMD_START);
	D("%s, worker write cmd start :%d\n", __func__, ret);

	if (s->callbacks.acquire_wakelock_cb)
		s->callbacks.acquire_wakelock_cb();

	return 0;
}


static int gps_iface_stop()
{
	GpsState *s = _gps_state;
	GpsNmea *nmea = _gps_nmea;
	int ret = 0;

	if (!s->initialized) {
		ALOGE("%s: called with uninitialized state !!", __FUNCTION__);
		return -1;
	}

	ret = nmea_write_control_command(nmea, CMD_STOP);
	D("%s, nmea write cmd stop :%d\n", __func__, ret);

	ret = write_control_command(s, CMD_STOP);
	D("%s, worker write cmd stop :%d\n", __func__, ret);

	if (s->callbacks.release_wakelock_cb)
		s->callbacks.release_wakelock_cb();

	return 0;
}


static int gps_iface_inject_time(GpsUtcTime time __unused,
								 int64_t timeReference __unused, int uncertainty __unused)
{
	V("%s", __FUNCTION__);
	return 0;
}

static int gps_iface_inject_location(double latitude __unused,
									 double longitude __unused, float accuracy __unused)
{
	V("%s", __FUNCTION__);
	return 0;
}

static void gps_iface_delete_aiding_data(GpsAidingData flags __unused)
{
	V("%s", __FUNCTION__);
#define FLAG_HOT_START  GPS_DELETE_RTI
#define FLAG_WARM_START GPS_DELETE_EPHEMERIS
#define FLAG_COLD_START (GPS_DELETE_EPHEMERIS | GPS_DELETE_POSITION | GPS_DELETE_TIME | GPS_DELETE_IONO | GPS_DELETE_UTC | GPS_DELETE_HEALTH)
#define FLAG_FULL_START (GPS_DELETE_ALL)
#define FLAG_AGPS_START (GPS_DELETE_EPHEMERIS | GPS_DELETE_ALMANAC | GPS_DELETE_POSITION | GPS_DELETE_TIME | GPS_DELETE_IONO | GPS_DELETE_UTC)
	GpsState*  s = _gps_state;
	int ret;

	if(!s->watch_enabled)
	{
		power_ctrl(1);
		sleep(1);
	}
	if ((flags == FLAG_FULL_START) || (flags == FLAG_COLD_START))
	{
		D("in %s, cold_start...",  __func__);
		ret = gps_send(&s->gps_data, "!NMEA_BOOT=COLD");
	}
	else if (flags == FLAG_WARM_START)
	{
		D("in %s, warm_start...",  __func__);
		ret = gps_send(&s->gps_data, "!NMEA_BOOT=WARM");
	}
	else
	{
		D("in %s, hot_start...",  __func__);
		ret = gps_send(&s->gps_data, "!NMEA_BOOT=HOT");
	}
	if(!s->watch_enabled)
	{
		sleep(1);
		power_ctrl(0);
	}
}

static int gps_iface_set_position_mode(GpsPositionMode mode __unused,
									   GpsPositionRecurrence recurrence, uint32_t min_interval,
									   uint32_t preferred_accuracy __unused,
									   uint32_t preferred_time __unused)
{
	GpsState *s = _gps_state;

	if (!s->initialized) {
		ALOGE("%s: called with uninitialized state !!", __FUNCTION__);
		return -1;
	}

	if (recurrence != GPS_POSITION_RECURRENCE_PERIODIC) {
		ALOGE("%s: recurrence %d not supported", __FUNCTION__, recurrence);
		return -1;
	}

	D("%s: mode=%d, recurrence=%d, min_interval=%d, "
	  "preferred_accuracy=%d, preferred_time=%d",
	  __FUNCTION__, mode, recurrence, min_interval,
	  preferred_accuracy, preferred_time);

	if(min_interval < 1000)
		min_interval = 1000;

	s->report_interval.tv_sec = min_interval / 1000;
	s->report_interval.tv_nsec = (min_interval % 1000) * 1000000;

	if((mode & 0x30) == 0x30)
		s->rnss_mode = NMEA_MODE_GNSS;
	else if((mode & 0x20) == 0x20)
		s->rnss_mode = NMEA_MODE_BDS;
	else if((mode & 0x10) == 0x10)
		s->rnss_mode = NMEA_MODE_GPS;

	return write_control_command(s, CMD_CHANGE_INTERVAL) > 0 ? 0 : -1;
}

/* GPS_DEBUG_INTERFACE */
static size_t gps_debug_iface_get_internal_state(char*, size_t);

static const void* gps_iface_get_extension(const char *name)
{
	static const GpsDebugInterface gps_debug_iface = {
		sizeof(GpsDebugInterface),
		gps_debug_iface_get_internal_state,
	};

	D("%s: %s", __FUNCTION__, name);
	if (0 == strcmp(name, GPS_DEBUG_INTERFACE))
		return &gps_debug_iface;

	return NULL;
}

static size_t gps_debug_iface_get_internal_state(char *buffer, size_t bufferSize)
{
	GpsState *s = _gps_state;

	if (!s->version)
		return 0;

	return strlcpy(buffer, s->version, bufferSize);
}

const GpsInterface* device_get_gps_interface()
{
	static const GpsInterface iface = {
		sizeof(GpsInterface),
		gps_iface_init,
		gps_iface_start,
		gps_iface_stop,
		gps_iface_cleanup,
		gps_iface_inject_time,
		gps_iface_inject_location,
		gps_iface_delete_aiding_data,
		gps_iface_set_position_mode,
		gps_iface_get_extension,
	};

	return &iface;
}

#if 0
static int device_close(struct hw_device_t *device __unused)
{
	V("%s: %p", __FUNCTION__, device);
	return 0;
}

static int module_open(const struct hw_module_t *module,
					   char const *name, struct hw_device_t **device)
{
	if (strcmp(name, GPS_HARDWARE_MODULE_ID) != 0)
		return -1;

	struct gps_device_t *dev = calloc(1, sizeof(struct gps_device_t));

	dev->common.tag = HARDWARE_DEVICE_TAG;
	dev->common.version = 0;
	dev->common.module = (struct hw_module_t*)module;
	dev->common.close = device_close;
	dev->get_gps_interface = device_get_gps_interface;

	*device = (struct hw_device_t*)dev;
	return 0;
}

static struct hw_module_methods_t gps_module_methods = {
	.open = module_open
};

struct hw_module_t HAL_MODULE_INFO_SYM = {
	.tag = HARDWARE_MODULE_TAG,
	.module_api_version = HARDWARE_MODULE_API_VERSION(0, 1),
	.hal_api_version = HARDWARE_HAL_API_VERSION,
	.id = GPS_HARDWARE_MODULE_ID,
	.name = "Catb.org gpsd GPS Module",
	.author = "You-Sheng Yang",
	.methods = &gps_module_methods,
};
int main(void){
	GpsState *s = _gps_state;
	reconnect_gpsd(s);
	return 0;
}
#endif
