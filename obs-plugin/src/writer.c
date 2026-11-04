#include "writer.h"

#include <obs-module.h>
#include <util/bmem.h>
#include <util/platform.h>
#include <util/threading.h>

#include "protocol.h"

/* 이보다 많이 쌓이면(느린 Wi-Fi에 큰 키프레임) 시간과 상관없이 버린다. */
#define MAX_MEDIA_BYTES (4 * 1024 * 1024)

struct out_packet {
	struct out_packet *next;
	uint64_t queued_ns;
	size_t size;
	uint8_t data[];
};

struct packet_queue {
	struct out_packet *head;
	struct out_packet *tail;
	size_t bytes;
};

struct pcam_writer {
	pcam_socket_t sock;
	pthread_t thread;
	pthread_mutex_t mutex;
	pthread_cond_t cond;
	struct packet_queue control;
	struct packet_queue media;
	bool waiting_keyframe; /* 모니터 패킷을 버린 뒤 다음 영상 CONFIG를 기다리는 중 */
	bool stopping;
	bool failed;
	long long dropped;
};

static void queue_push(struct packet_queue *q, struct out_packet *p)
{
	p->next = NULL;
	if (q->tail)
		q->tail->next = p;
	else
		q->head = p;
	q->tail = p;
	q->bytes += p->size;
}

static struct out_packet *queue_pop(struct packet_queue *q)
{
	struct out_packet *p = q->head;
	if (!p)
		return NULL;
	q->head = p->next;
	if (!q->head)
		q->tail = NULL;
	q->bytes -= p->size;
	return p;
}

static void queue_clear(struct packet_queue *q)
{
	struct out_packet *p;
	while ((p = queue_pop(q)) != NULL)
		bfree(p);
}

static struct out_packet *make_packet(uint8_t stream_id, uint8_t flags, int64_t pts_us, const void *payload,
				      size_t size)
{
	struct out_packet *p = bmalloc(sizeof(*p) + PCAM_HEADER_SIZE + size);
	p->queued_ns = os_gettime_ns();
	p->size = PCAM_HEADER_SIZE + size;
	pcam_header_encode(stream_id, (uint32_t)size, p->data);
	p->data[1] = flags;
	uint64_t pts = (uint64_t)pts_us;
	for (int i = 0; i < 8; i++)
		p->data[8 + i] = (uint8_t)(pts >> (56 - 8 * i));
	if (size)
		memcpy(p->data + PCAM_HEADER_SIZE, payload, size);
	return p;
}

static void *writer_main(void *param)
{
	struct pcam_writer *w = param;
	os_set_thread_name("phonecam-writer");

	pthread_mutex_lock(&w->mutex);
	for (;;) {
		while (!w->stopping && !w->control.head && !w->media.head)
			pthread_cond_wait(&w->cond, &w->mutex);
		if (w->stopping)
			break;
		struct out_packet *p = queue_pop(&w->control);
		if (!p)
			p = queue_pop(&w->media);
		pthread_mutex_unlock(&w->mutex);

		bool sent = pcam_send_all(w->sock, p->data, p->size);
		bfree(p);

		pthread_mutex_lock(&w->mutex);
		if (!sent) {
			/* 받는 스레드도 recv에서 깨어나 연결을 정리한다. */
			w->failed = true;
			queue_clear(&w->control);
			queue_clear(&w->media);
			pcam_shutdown(w->sock);
			break;
		}
	}
	pthread_mutex_unlock(&w->mutex);
	return NULL;
}

struct pcam_writer *pcam_writer_start(pcam_socket_t sock)
{
	struct pcam_writer *w = bzalloc(sizeof(*w));
	w->sock = sock;
	pthread_mutex_init(&w->mutex, NULL);
	pthread_cond_init(&w->cond, NULL);
	if (pthread_create(&w->thread, NULL, writer_main, w) != 0) {
		pthread_cond_destroy(&w->cond);
		pthread_mutex_destroy(&w->mutex);
		bfree(w);
		return NULL;
	}
	return w;
}

void pcam_writer_stop(struct pcam_writer *w)
{
	if (!w)
		return;
	pthread_mutex_lock(&w->mutex);
	w->stopping = true;
	pthread_cond_signal(&w->cond);
	pthread_mutex_unlock(&w->mutex);
	pcam_shutdown(w->sock); /* 느린 Wi-Fi에서 send에 묶여 있으면 깨운다 */
	pthread_join(w->thread, NULL);

	if (w->dropped)
		blog(LOG_INFO, "[phonecam] monitor: dropped %lld packets on a slow connection", w->dropped);
	queue_clear(&w->control);
	queue_clear(&w->media);
	pthread_cond_destroy(&w->cond);
	pthread_mutex_destroy(&w->mutex);
	bfree(w);
}

void pcam_writer_control(struct pcam_writer *w, const char *json, size_t len)
{
	struct out_packet *p = make_packet(PCAM_STREAM_CONTROL, 0, 0, json, len);
	pthread_mutex_lock(&w->mutex);
	if (w->failed || w->stopping) {
		bfree(p);
	} else {
		queue_push(&w->control, p);
		pthread_cond_signal(&w->cond);
	}
	pthread_mutex_unlock(&w->mutex);
}

void pcam_writer_media(struct pcam_writer *w, uint8_t stream_id, uint8_t flags, int64_t pts_us, const uint8_t *data,
		       size_t size)
{
	uint64_t now = os_gettime_ns();
	pthread_mutex_lock(&w->mutex);
	if (w->failed || w->stopping)
		goto done;

	struct out_packet *oldest = w->media.head;
	bool late = oldest && now - oldest->queued_ns > (uint64_t)PCAM_WRITER_MAX_DELAY_MS * 1000000ULL;
	if (late || w->media.bytes + size > MAX_MEDIA_BYTES) {
		for (struct out_packet *p = w->media.head; p; p = p->next)
			w->dropped++;
		queue_clear(&w->media);
		w->waiting_keyframe = true;
	}
	if (w->waiting_keyframe) {
		bool video_config = stream_id == PCAM_STREAM_MONITOR_VIDEO && (flags & PCAM_FLAG_CONFIG);
		if (!video_config) {
			w->dropped++;
			goto done;
		}
		w->waiting_keyframe = false;
	}
	queue_push(&w->media, make_packet(stream_id, flags, pts_us, data, size));
	pthread_cond_signal(&w->cond);
done:
	pthread_mutex_unlock(&w->mutex);
}
