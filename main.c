#include <stdlib.h>  
#include <stdio.h>  
#include <string.h>  
#include <math.h>
#include <unistd.h>

#include <libavutil/imgutils.h>  
#include <libavutil/avutil.h> // 可能还需要这个，用于av_malloc等函数
#include <libavutil/avassert.h>  
#include <libavutil/channel_layout.h>  
#include <libavutil/opt.h>  
#include <libavutil/mathematics.h>  
#include <libavutil/timestamp.h>  
#include <libavformat/avformat.h>  
#include <libswscale/swscale.h>  
#include <libswresample/swresample.h>  

#define STREAM_DURATION   50.0 /*录制视频的持续时间  秒*/
#define STREAM_FRAME_RATE 30 	/* images/s  这里可以根据摄像头的采集速度来设置帧率 */
#define STREAM_PIX_FMT    AV_PIX_FMT_YUV420P /* default pix_fmt */  
#define SCALE_FLAGS SWS_BICUBIC  

//存放视频的宽度和高度
int video_width;
int video_height;

// 单个输出AVStream的包装器 
typedef struct OutputStream
{
	AVStream* st;
	AVCodecContext* enc;
	/*下一帧的点数*/
	int64_t next_pts;
	int samples_count;
	AVFrame* frame;
	AVFrame* tmp_frame;
	float t, tincr, tincr2;
	struct SwsContext* sws_ctx;
	struct SwrContext* swr_ctx;
}OutputStream;


typedef struct IntputDev
{
	AVCodecContext* pCodecCtx;
	AVCodec* pCodec;
	AVFormatContext* v_ifmtCtx;
	int  videoindex;
	struct SwsContext* img_convert_ctx;
	AVPacket* in_packet;
	AVFrame* pFrame, * pFrameYUV;
}IntputDev;

static void log_packet(const AVFormatContext* fmt_ctx, const AVPacket* pkt)
{
	AVRational* time_base = &fmt_ctx->streams[pkt->stream_index]->time_base;
}

static int write_frame(AVFormatContext* fmt_ctx, const AVRational* time_base, AVStream* st, AVPacket* pkt)
{
	/* 将输出数据包时间戳值从编解码器重新调整为流时基 */
	av_packet_rescale_ts(pkt, *time_base, st->time_base);
	pkt->stream_index = st->index;

	/*将压缩的帧写入媒体文件。*/
	log_packet(fmt_ctx, pkt);
	return av_interleaved_write_frame(fmt_ctx, pkt);
}

/*添加输出流。 */
static void add_stream(OutputStream* ost, AVFormatContext* oc, AVCodec** codec, enum AVCodecID codec_id)
{
	AVCodecContext* c;
	int i;
	/* find the encoder */

	*codec = avcodec_find_encoder(codec_id);
	if (!(*codec))
	{
		fprintf(stderr, "Could not find encoder for '%s'\n",
			avcodec_get_name(codec_id));
		exit(1);
	}
	ost->st = avformat_new_stream(oc, NULL);

	if (!ost->st) {
		fprintf(stderr, "Could not allocate stream\n");
		exit(1);
	}
	ost->st->id = oc->nb_streams - 1;
	c = avcodec_alloc_context3(*codec);
	if (!c) {
		fprintf(stderr, "Could not alloc an encoding context\n");
		exit(1);
	}
	ost->enc = c;
	switch ((*codec)->type)
	{
	case AVMEDIA_TYPE_AUDIO:
		c->sample_fmt = (*codec)->sample_fmts ?
			(*codec)->sample_fmts[0] : AV_SAMPLE_FMT_FLTP;
		c->bit_rate = 64000;
		c->sample_rate = 44100;
		if ((*codec)->supported_samplerates) {
			c->sample_rate = (*codec)->supported_samplerates[0];
			for (i = 0; (*codec)->supported_samplerates[i]; i++) {
				if ((*codec)->supported_samplerates[i] == 44100)
					c->sample_rate = 44100;
			}
		}
		c->channels = av_get_channel_layout_nb_channels(c->channel_layout);
		c->channel_layout = AV_CH_LAYOUT_STEREO;
		if ((*codec)->channel_layouts) {
			c->channel_layout = (*codec)->channel_layouts[0];
			for (i = 0; (*codec)->channel_layouts[i]; i++) {
				if ((*codec)->channel_layouts[i] == AV_CH_LAYOUT_STEREO)
					c->channel_layout = AV_CH_LAYOUT_STEREO;
			}
		}
		c->channels = av_get_channel_layout_nb_channels(c->channel_layout);
		ost->st->time_base = (AVRational){ 1, c->sample_rate };
		break;

	case AVMEDIA_TYPE_VIDEO:
		c->codec_id = codec_id;
		c->bit_rate = 2500000;  //平均比特率,例子代码默认值是400000
		/* 分辨率必须是2的倍数。*/
		c->width = video_width;
		c->height = video_height;
		/*时基：这是基本的时间单位（以秒为单位）
		 *表示其中的帧时间戳。 对于固定fps内容，
		 *时基应为1 /framerate，时间戳增量应为
		 *等于1。*/
		ost->st->time_base = (AVRational){ 1,STREAM_FRAME_RATE };  //帧率设置
		c->time_base = ost->st->time_base;
		c->gop_size = 12; /* 最多每十二帧发射一帧内帧 */
		c->pix_fmt = STREAM_PIX_FMT;
		if (c->codec_id == AV_CODEC_ID_MPEG2VIDEO)
		{
			/* 只是为了测试，我们还添加了B帧 */
			c->max_b_frames = 2;
		}
		if (c->codec_id == AV_CODEC_ID_MPEG1VIDEO)
		{
			/*需要避免使用其中一些系数溢出的宏块。
			 *普通视频不会发生这种情况，因为
			 *色度平面的运动与亮度平面不匹配。 */
			c->mb_decision = 2;
		}
		break;

	default:
		break;
	}

	/* 某些格式希望流头分开。 */
	if (oc->oformat->flags & AVFMT_GLOBALHEADER)
		c->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
}

static AVFrame* alloc_picture(enum AVPixelFormat pix_fmt, int width, int height)
{
	AVFrame* picture;
	int ret;
	picture = av_frame_alloc();
	if (!picture)
		return NULL;
	picture->format = pix_fmt;
	picture->width = width;
	picture->height = height;

	/* 为帧数据分配缓冲区 */
	ret = av_frame_get_buffer(picture, 32);
	if (ret < 0)
	{
		fprintf(stderr, "Could not allocate frame data.\n");
		exit(1);
	}
	return picture;
}

static void open_video(AVFormatContext* oc, AVCodec* codec, OutputStream* ost, AVDictionary* opt_arg)
{
	int ret;
	AVCodecContext* c = ost->enc;
	AVDictionary* opt = NULL;

	av_dict_copy(&opt, opt_arg, 0);

	/* open the codec */
	ret = avcodec_open2(c, codec, &opt);
	av_dict_free(&opt);
	if (ret < 0)
	{
		fprintf(stderr, "Could not open video codec\n");
		exit(1);
	}

	/* 分配并初始化可重用框架 */
	ost->frame = alloc_picture(c->pix_fmt, c->width, c->height);
	if (!ost->frame)
	{
		fprintf(stderr, "Could not allocate video frame\n");
		exit(1);
	}
	printf("ost->frame alloc success fmt=%d w=%d h=%d\n", c->pix_fmt, c->width, c->height);


	/*如果输出格式不是YUV420P，则为临时YUV420P
	*也需要图片。 然后将其转换为所需的
	*输出格式。 */
	ost->tmp_frame = NULL;
	if (c->pix_fmt != AV_PIX_FMT_YUV420P)
	{
		ost->tmp_frame = alloc_picture(AV_PIX_FMT_YUV420P, c->width, c->height);
		if (!ost->tmp_frame)
		{
			fprintf(stderr, "Could not allocate temporary picture\n");
			exit(1);
		}
	}

	/* 将流参数复制到多路复用器*/
	ret = avcodec_parameters_from_context(ost->st->codecpar, c);
	if (ret < 0)
	{
		fprintf(stderr, "Could not copy the stream parameters\n");
		exit(1);
	}
}

/*
  *编码一个视频帧
  *编码完成后返回1，否则返回0
  */
static int write_video_frame(AVFormatContext* oc, OutputStream* ost, AVFrame* frame)
{
	int ret;
	AVCodecContext* c;
	int got_packet = 0;
	AVPacket pkt = { 0 };
	if (frame == NULL)
		return 1;
	c = ost->enc;
	av_init_packet(&pkt);
	/* 编码图像*/
	ret = avcodec_encode_video2(c, &pkt, frame, &got_packet);
	if (ret < 0)
	{
		fprintf(stderr, "Error encoding video frame\n");
		exit(1);
	}
	printf("--------------video- pkt.pt\n");
	printf("----st.num=%d st.den=%d codec.num=%d codec.den=%d---------\n", ost->st->time_base.num, ost->st->time_base.den,
		c->time_base.num, c->time_base.den);
	if (got_packet)
	{
		ret = write_frame(oc, &c->time_base, ost->st, &pkt);
	}
	else
	{
		ret = 0;
	}
	if (ret < 0)
	{
		fprintf(stderr, "Error while writing video frame\n");
		exit(1);
	}
	return (frame || got_packet) ? 0 : 1;
}


static AVFrame* get_video_frame(OutputStream* ost, IntputDev* input, int* got_pic)
{
	int ret, got_picture;
	AVCodecContext* c = ost->enc;
	AVFrame* ret_frame = NULL;
	if (av_compare_ts(ost->next_pts, c->time_base, STREAM_DURATION, (AVRational) { 1, 1 }) >= 0)
		return NULL;

	/*当我们将帧传递给编码器时，它可能会保留对它的引用
	*内部,确保我们在这里不覆盖它*/
	if (av_frame_make_writable(ost->frame) < 0)
		exit(1);
	if (av_read_frame(input->v_ifmtCtx, input->in_packet) >= 0)
	{
		if (input->in_packet->stream_index == input->videoindex)
		{
			ret = avcodec_decode_video2(input->pCodecCtx, input->pFrame, &got_picture, input->in_packet);
			*got_pic = got_picture;
			if (ret < 0)
			{
				printf("Decode Error.\n");
				av_packet_unref(input->in_packet);
				return NULL;
			}
			if (got_picture)
			{
				sws_scale(input->img_convert_ctx, (const unsigned char* const*)input->pFrame->data, input->pFrame->linesize, 0, input->pCodecCtx->height, ost->frame->data, ost->frame->linesize);
				ost->frame->pts = ost->next_pts++;
				ret_frame = ost->frame;
			}
		}
		av_packet_unref(input->in_packet);
	}
	return ret_frame;
}
static void close_stream(AVFormatContext* oc, OutputStream* ost)
{
	avcodec_free_context(&ost->enc);
	av_frame_free(&ost->frame);
	av_frame_free(&ost->tmp_frame);
	sws_freeContext(ost->sws_ctx);
	swr_free(&ost->swr_ctx);
}

/*
采集摄像头数据编码成MP4视频
*/
int main(int argc, char** argv)
{
	OutputStream video_st = { 0 }, audio_st = { 0 };
	const char* filename;
	AVOutputFormat* fmt;
	AVFormatContext* oc;
	AVCodec* audio_codec, * video_codec;
	int ret;
	int have_video = 0, have_audio = 0;
	int encode_video = 0, encode_audio = 0;
	AVDictionary* opt = NULL;
	int i;

	if (argc < 3)
	{
		//./app /dev/video0 123.mp4
		printf("Usage: %s <Camera device node> <file name> \n", argv[0]);
		return 1;
	}

	filename = argv[2];
	printf("The name of the currently stored video file:%s\n", filename);
	/*分配输出媒体环境*/
	avformat_alloc_output_context2(&oc, NULL, NULL, filename);
	if (!oc)
	{
		printf("The output format cannot be inferred from the file extension：Use MPEG。\n");
		avformat_alloc_output_context2(&oc, NULL, "mpeg", filename);
	}
	if (!oc)return 1;
	//添加摄像头----------------------------------
	IntputDev video_input = { 0 };
	AVCodecContext* pCodecCtx;
	AVCodec* pCodec;
	AVFormatContext* v_ifmtCtx;
	//avdevice_register_all();
	v_ifmtCtx = avformat_alloc_context();
	//Linux下指定摄像头信息  
	AVInputFormat* ifmt = av_find_input_format("video4linux2");
	if (avformat_open_input(&v_ifmtCtx, argv[1], ifmt, NULL) != 0)
	{
		printf("Unable to open the input stream.%s\n", argv[1]);
		return -1;
	}
	if (avformat_find_stream_info(v_ifmtCtx, NULL) < 0)
	{
		printf("Stream information could not be found.\n");
		return -1;
	}
	int videoindex = -1;
	for (i = 0; i < v_ifmtCtx->nb_streams; i++)
		if (v_ifmtCtx->streams[i]->codec->codec_type == AVMEDIA_TYPE_VIDEO)
		{
			videoindex = i;
			printf("videoindex=%d\n", videoindex);
			break;
		}
	if (videoindex == -1)
	{
		printf("The video stream could not be found。\n");
		return -1;
	}
	pCodecCtx = v_ifmtCtx->streams[videoindex]->codec;
	pCodec = avcodec_find_decoder(pCodecCtx->codec_id);
	if (pCodec == NULL)
	{
		printf("Codec not found\n");
		return -1;
	}
	if (avcodec_open2(pCodecCtx, pCodec, NULL) < 0)
	{
		printf("The codec could not be opened。\n");
		return -1;
	}

	AVFrame* pFrame, * pFrameYUV;
	pFrame = av_frame_alloc();
	pFrameYUV = av_frame_alloc();

	unsigned char* out_buffer = (unsigned char*)av_malloc(av_image_get_buffer_size(AV_PIX_FMT_YUV420P, pCodecCtx->width, pCodecCtx->height, 16));

	av_image_fill_arrays(pFrameYUV->data, 
						 pFrameYUV->linesize,
						out_buffer, 
						AV_PIX_FMT_YUV420P,
						pCodecCtx->width, 
						pCodecCtx->height, 16);

	//av_image_fill_arrays((AVPicture*)pFrameYUV->data, (AVPicture*)pFrameYUV->linesize, out_buffer, AV_PIX_FMT_YUV420P, pCodecCtx->width, pCodecCtx->height, 16);

	printf("Camera size(WxH): %d x %d \n", pCodecCtx->width, pCodecCtx->height);
	video_width = pCodecCtx->width;
	video_height = pCodecCtx->height;
	struct SwsContext* img_convert_ctx;
	img_convert_ctx = sws_getContext(pCodecCtx->width, pCodecCtx->height, pCodecCtx->pix_fmt, pCodecCtx->width, pCodecCtx->height, AV_PIX_FMT_YUV420P, SWS_BICUBIC, NULL, NULL, NULL);
	AVPacket* in_packet = (AVPacket*)av_malloc(sizeof(AVPacket));
	video_input.img_convert_ctx = img_convert_ctx;
	video_input.in_packet = in_packet;
	video_input.pCodecCtx = pCodecCtx;
	video_input.pCodec = pCodec;
	video_input.v_ifmtCtx = v_ifmtCtx;
	video_input.videoindex = videoindex;
	video_input.pFrame = pFrame;
	video_input.pFrameYUV = pFrameYUV;
	//-----------------------------添加摄像头结束
	fmt = oc->oformat;
	/*使用默认格式的编解码器添加音频和视频流并初始化编解码器。*/
	printf("fmt->video_codec = %d\n", fmt->video_codec);
	if (fmt->video_codec != AV_CODEC_ID_NONE)
	{
		add_stream(&video_st, oc, &video_codec, fmt->video_codec);
		have_video = 1;
		encode_video = 1;
	}
	/*现在已经设置了所有参数，可以打开音频并视频编解码器，并分配必要的编码缓冲区。*/
	if (have_video)open_video(oc, video_codec, &video_st, opt);
	av_dump_format(oc, 0, filename, 1);
	/* 打开输出文件(如果需要) */
	if (!(fmt->flags & AVFMT_NOFILE))
	{
		ret = avio_open(&oc->pb, filename, AVIO_FLAG_WRITE);
		if (ret < 0)
		{
			fprintf(stderr, "Can't open'%s'\n", filename);
			return 1;
		}
	}
	/* 编写流头(如果有)*/
	ret = avformat_write_header(oc, &opt);
	if (ret < 0)
	{
		fprintf(stderr, "An error occurred while opening the output file\n");
		return 1;
	}
	int got_pic;
	while (encode_video)
	{
		/*选择要编码的流*/
		AVFrame* frame = get_video_frame(&video_st, &video_input, &got_pic);
		if (!got_pic)
		{
			usleep(10000);
			continue;
		}
		encode_video = !write_video_frame(oc, &video_st, frame);
	}
	av_write_trailer(oc);
	sws_freeContext(video_input.img_convert_ctx);
	avcodec_close(video_input.pCodecCtx);
	av_free(video_input.pFrameYUV);
	av_free(video_input.pFrame);
	avformat_close_input(&video_input.v_ifmtCtx);
	/*关闭每个编解码器*/
	if (have_video)close_stream(oc, &video_st);
	/*关闭输出文件*/
	if (!(fmt->flags & AVFMT_NOFILE))avio_closep(&oc->pb);
	/*释放流*/
	avformat_free_context(oc);
	return 0;
}