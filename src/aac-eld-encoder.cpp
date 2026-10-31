#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif

#include <aacenc_lib.h>

namespace {

constexpr int kSampleRate = 44100;
constexpr int kChannels = 2;
constexpr int kSamplesPerFrame = 480;
constexpr int kBytesPerSample = 2;
constexpr int kPCMFrameBytes =
	kSamplesPerFrame * kChannels * kBytesPerSample;
constexpr int kMaxEncodedBytes = 8192;

bool write_all(const void *data, size_t size)
{
	const auto *bytes = static_cast<const std::uint8_t *>(data);
	while (size > 0) {
		const size_t written = std::fwrite(bytes, 1, size, stdout);
		if (written == 0)
			return false;
		bytes += written;
		size -= written;
	}
	return true;
}

void write_u32_le(std::uint32_t value)
{
	const std::array<std::uint8_t, 4> bytes = {
		static_cast<std::uint8_t>(value),
		static_cast<std::uint8_t>(value >> 8),
		static_cast<std::uint8_t>(value >> 16),
		static_cast<std::uint8_t>(value >> 24),
	};
	write_all(bytes.data(), bytes.size());
}

bool fail(AACENC_ERROR error, const char *operation)
{
	std::fprintf(stderr, "AAC-ELD %s failed: FDK error %d\n", operation,
		     static_cast<int>(error));
	return false;
}

} // namespace

int main()
{
#ifdef _WIN32
	_setmode(_fileno(stdin), _O_BINARY);
	_setmode(_fileno(stdout), _O_BINARY);
#endif
	std::setvbuf(stdout, nullptr, _IONBF, 0);

	HANDLE_AACENCODER encoder = nullptr;
	AACENC_ERROR error = aacEncOpen(&encoder, 0, kChannels);
	if (error != AACENC_OK)
		return fail(error, "open") ? 0 : 1;

	struct EncoderCloser {
		HANDLE_AACENCODER *encoder;
		~EncoderCloser()
		{
			if (*encoder != nullptr)
				aacEncClose(encoder);
		}
	} closer{&encoder};

	const struct {
		AACENC_PARAM parameter;
		UINT value;
		const char *name;
	} settings[] = {
		{AACENC_AOT, AOT_ER_AAC_ELD, "AOT"},
		{AACENC_SAMPLERATE, kSampleRate, "sample rate"},
		{AACENC_CHANNELMODE, MODE_2, "channel mode"},
		{AACENC_CHANNELORDER, 1, "channel order"},
		{AACENC_BITRATE, 256000, "bitrate"},
		{AACENC_TRANSMUX, TT_MP4_RAW, "transmux"},
		{AACENC_SBR_MODE, 0, "SBR mode"},
		{AACENC_GRANULE_LENGTH, kSamplesPerFrame, "granule length"},
		{AACENC_AFTERBURNER, 1, "afterburner"},
	};
	for (const auto &setting : settings) {
		error = aacEncoder_SetParam(encoder, setting.parameter, setting.value);
		if (error != AACENC_OK) {
			std::fprintf(stderr, "AAC-ELD setting %s=%u failed: FDK error %d\n",
				     setting.name, setting.value, static_cast<int>(error));
			return 1;
		}
	}

	error = aacEncEncode(encoder, nullptr, nullptr, nullptr, nullptr);
	if (error != AACENC_OK)
		return fail(error, "initialize") ? 0 : 1;

	AACENC_InfoStruct info = {};
	error = aacEncInfo(encoder, &info);
	if (error != AACENC_OK)
		return fail(error, "read info") ? 0 : 1;
	if (info.frameLength != kSamplesPerFrame) {
		std::fprintf(stderr, "AAC-ELD selected %u samples per frame, want %d\n",
			     info.frameLength, kSamplesPerFrame);
		return 1;
	}

	std::printf("READY %d\n", kSamplesPerFrame);
	std::fflush(stdout);

	std::array<std::uint8_t, kPCMFrameBytes> pcm = {};
	std::array<std::uint8_t, kMaxEncodedBytes> output = {};
	for (;;) {
		const size_t read = std::fread(pcm.data(), 1, pcm.size(), stdin);
		if (read == 0 && std::feof(stdin))
			return 0;
		if (read != pcm.size()) {
			std::fprintf(stderr, "AAC-ELD PCM read returned %zu bytes\n", read);
			return 1;
		}

		AACENC_BufDesc input_description = {};
		AACENC_BufDesc output_description = {};
		AACENC_InArgs input_arguments = {};
		AACENC_OutArgs output_arguments = {};
		void *input_buffers[] = {pcm.data()};
		void *output_buffers[] = {output.data()};
		INT input_identifiers[] = {IN_AUDIO_DATA};
		INT output_identifiers[] = {OUT_BITSTREAM_DATA};
		INT input_sizes[] = {static_cast<INT>(pcm.size())};
		INT output_sizes[] = {static_cast<INT>(output.size())};
		INT input_element_sizes[] = {static_cast<INT>(sizeof(INT_PCM))};
		INT output_element_sizes[] = {1};

		input_description.numBufs = 1;
		input_description.bufs = input_buffers;
		input_description.bufferIdentifiers = input_identifiers;
		input_description.bufSizes = input_sizes;
		input_description.bufElSizes = input_element_sizes;

		output_description.numBufs = 1;
		output_description.bufs = output_buffers;
		output_description.bufferIdentifiers = output_identifiers;
		output_description.bufSizes = output_sizes;
		output_description.bufElSizes = output_element_sizes;

		input_arguments.numInSamples = kSamplesPerFrame * kChannels;
		error = aacEncEncode(encoder, &input_description, &output_description,
				     &input_arguments, &output_arguments);
		if (error != AACENC_OK)
			return fail(error, "encode") ? 0 : 1;
		if (output_arguments.numOutBytes <= 0 ||
		    output_arguments.numOutBytes > kMaxEncodedBytes) {
			std::fprintf(stderr, "AAC-ELD returned invalid output size %d\n",
				     output_arguments.numOutBytes);
			return 1;
		}

		write_u32_le(static_cast<std::uint32_t>(output_arguments.numOutBytes));
		if (!write_all(output.data(),
			       static_cast<size_t>(output_arguments.numOutBytes)))
			return 1;
	}
}
