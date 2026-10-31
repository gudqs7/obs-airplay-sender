#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#include <malloc.h>
#endif

extern "C" {
#include "ixheaac_error_standards.h"
#include "ixheaac_type_def.h"
#include "ixheaace_api.h"
}

namespace {

constexpr int kSampleRate = 44100;
constexpr int kChannels = 2;
constexpr int kSamplesPerFrame = 480;
constexpr int kBytesPerSample = 2;
constexpr int kPCMFrameBytes =
	kSamplesPerFrame * kChannels * kBytesPerSample;
constexpr int kBitrate = 256000;
constexpr int kExpectedASCBytes = 4;

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

bool write_u32_le(std::uint32_t value)
{
	const std::array<std::uint8_t, 4> bytes = {
		static_cast<std::uint8_t>(value),
		static_cast<std::uint8_t>(value >> 8),
		static_cast<std::uint8_t>(value >> 16),
		static_cast<std::uint8_t>(value >> 24),
	};
	return write_all(bytes.data(), bytes.size());
}

pVOID aligned_malloc(UWORD32 size, UWORD32 alignment)
{
#ifdef _WIN32
	return _aligned_malloc(size, alignment);
#else
	void *ptr = nullptr;
	if (posix_memalign(&ptr, alignment, size) != 0)
		return nullptr;
	return ptr;
#endif
}

VOID aligned_free(pVOID ptr)
{
#ifdef _WIN32
	_aligned_free(ptr);
#else
	std::free(ptr);
#endif
}

void configure_encoder(ixheaace_input_config &input, ixheaace_output_config &output)
{
	std::memset(&input, 0, sizeof(input));
	std::memset(&output, 0, sizeof(output));

	input.ui_pcm_wd_sz = 16;
	input.i_bitrate = kBitrate;
	input.frame_length = kSamplesPerFrame;
	input.aot = AOT_AAC_ELD;
	input.i_channels = kChannels;
	input.i_samp_freq = kSampleRate;
	input.i_use_mps = 0;
	input.i_mps_tree_config = -1;
	input.i_use_adts = 0;
	input.i_use_es = 1;

	input.aac_config.bitrate = kBitrate;
	input.aac_config.use_tns = 1;
	input.aac_config.inv_quant = 2;
	input.aac_config.bitreservoir_size = -1;

	output.malloc_xheaace = aligned_malloc;
	output.free_xheaace = aligned_free;
}

} // namespace

int main()
{
#ifdef _WIN32
	_setmode(_fileno(stdin), _O_BINARY);
	_setmode(_fileno(stdout), _O_BINARY);
#endif
	std::setvbuf(stdout, nullptr, _IONBF, 0);

	ixheaace_input_config input = {};
	ixheaace_output_config output = {};
	configure_encoder(input, output);

	IA_ERRORCODE error = ixheaace_create(&input, &output);
	if (error != IA_NO_ERROR) {
		std::fprintf(stderr, "AAC-ELD libxaac open failed: 0x%08x\n",
			     static_cast<unsigned int>(error));
		return 1;
	}

	struct EncoderCloser {
		ixheaace_output_config *output;
		~EncoderCloser()
		{
			if (output->pv_ia_process_api_obj != nullptr)
				ixheaace_delete(output);
		}
	} closer{&output};

	if (output.input_size != kPCMFrameBytes ||
	    output.samp_freq != kSampleRate || output.down_sampling_ratio != 1.0f ||
	    output.i_out_bytes != kExpectedASCBytes) {
		std::fprintf(
			stderr,
			"AAC-ELD libxaac selected an incompatible mode: "
			"input=%d rate=%d ratio=%.2f asc=%d\n",
			output.input_size, output.samp_freq,
			output.down_sampling_ratio, output.i_out_bytes);
		return 1;
	}

	auto *input_buffer = static_cast<WORD16 *>(
		output.mem_info_table[IA_MEMTYPE_INPUT].mem_ptr);
	const auto *output_buffer = static_cast<const std::uint8_t *>(
		output.mem_info_table[IA_MEMTYPE_OUTPUT].mem_ptr);

	std::printf("READY %d\n", kSamplesPerFrame);
	std::fflush(stdout);

	std::array<std::uint8_t, kPCMFrameBytes> pcm = {};
	for (;;) {
		const size_t read = std::fread(pcm.data(), 1, pcm.size(), stdin);
		if (read == 0 && std::feof(stdin))
			return 0;
		if (read != pcm.size()) {
			std::fprintf(stderr, "AAC-ELD libxaac PCM read returned %zu bytes\n",
				     read);
			return 1;
		}

		std::memcpy(input_buffer, pcm.data(), pcm.size());
		error = ixheaace_process(output.pv_ia_process_api_obj, &input, &output);
		if (error != IA_NO_ERROR || output.i_out_bytes <= 0) {
			std::fprintf(stderr,
				     "AAC-ELD libxaac encode failed: 0x%08x output=%d\n",
				     static_cast<unsigned int>(error), output.i_out_bytes);
			return 1;
		}

		if (!write_u32_le(static_cast<std::uint32_t>(output.i_out_bytes)) ||
		    !write_all(output_buffer, static_cast<size_t>(output.i_out_bytes)))
			return 1;
	}
}
