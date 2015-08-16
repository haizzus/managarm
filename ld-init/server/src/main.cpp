
#include <frigg/types.hpp>
#include <frigg/traits.hpp>
#include <frigg/initializer.hpp>
#include <frigg/support.hpp>
#include <frigg/debug.hpp>
#include <frigg/memory.hpp>
#include <frigg/libc.hpp>
#include <frigg/string.hpp>
#include <frigg/variant.hpp>
#include <frigg/vector.hpp>
#include <frigg/hashmap.hpp>
#include <frigg/callback.hpp>
#include <frigg/async.hpp>
#include <frigg/elf.hpp>

#include <hel.h>
#include <hel-syscalls.h>
#include <helx.hpp>

#include <frigg/glue-hel.hpp>

#include <frigg/protobuf.hpp>
#include <bragi-naked/ld-server.nakedpb.hpp>

namespace debug = frigg::debug;
namespace util = frigg::util;
namespace memory = frigg::memory;
namespace async = frigg::async;
namespace protobuf = frigg::protobuf;

struct BaseSegment {
	BaseSegment(Elf64_Word elf_type, Elf64_Word elf_flags,
			uintptr_t virt_address, size_t virt_length)
	: elfType(elf_type), elfFlags(elf_flags),
			virtAddress(virt_address), virtLength(virt_length) { }

	Elf64_Word elfType;
	Elf64_Word elfFlags;
	uintptr_t virtAddress;
	size_t virtLength;
};

struct SharedSegment : BaseSegment {
	SharedSegment(Elf64_Word elf_type, Elf64_Word elf_flags,
			uintptr_t virt_address, size_t virt_length, HelHandle memory)
	: BaseSegment(elf_type, elf_flags, virt_address, virt_length),
			memory(memory) { }
	
	HelHandle memory;
};

struct UniqueSegment : BaseSegment {
	UniqueSegment(Elf64_Word elf_type, Elf64_Word elf_flags,
			uintptr_t virt_address, size_t virt_length,
			uintptr_t file_disp, uintptr_t file_offset, size_t file_length)
	: BaseSegment(elf_type, elf_flags, virt_address, virt_length),
			fileDisplacement(file_disp), fileOffset(file_offset),
			fileLength(file_length) { }
	
	uintptr_t fileDisplacement;
	uintptr_t fileOffset;
	size_t fileLength;
};

typedef util::Variant<SharedSegment,
		UniqueSegment> Segment;

struct Object {
	Object() : entry(0), dynamic(0), segments(*allocator) { }

	void *imagePtr;
	uintptr_t entry;
	uintptr_t dynamic;
	util::Vector<Segment, Allocator> segments;
};

typedef util::Hashmap<const char *, Object *,
		util::CStringHasher, Allocator> objectMap;

Object *readObject(util::StringView path) {
	util::String<Allocator> full_path(*allocator, "initrd/");
	full_path += path;

	// open and map the executable image into this address space
	HelHandle image_handle;
	helRdOpen(full_path.data(), full_path.size(), &image_handle);

	size_t image_size;
	void *image_ptr;
	helMemoryInfo(image_handle, &image_size);
	helMapMemory(image_handle, kHelNullHandle, nullptr, image_size,
			kHelMapReadOnly, &image_ptr);
	
	constexpr size_t kPageSize = 0x1000;
	
	// parse the ELf file format
	Elf64_Ehdr *ehdr = (Elf64_Ehdr*)image_ptr;
	ASSERT(ehdr->e_ident[0] == 0x7F
			&& ehdr->e_ident[1] == 'E'
			&& ehdr->e_ident[2] == 'L'
			&& ehdr->e_ident[3] == 'F');
	ASSERT(ehdr->e_type == ET_EXEC || ehdr->e_type == ET_DYN);

	Object *object = memory::construct<Object>(*allocator);
	object->imagePtr = image_ptr;
	object->entry = ehdr->e_entry;

	for(int i = 0; i < ehdr->e_phnum; i++) {
		auto phdr = (Elf64_Phdr *)((uintptr_t)image_ptr + ehdr->e_phoff
				+ i * ehdr->e_phentsize);

		if(phdr->p_type == PT_LOAD) {
			bool shared = false;
			if((phdr->p_flags & (PF_R | PF_W | PF_X)) == (PF_R | PF_W)) {
				// we cannot share this segment
			}else if((phdr->p_flags & (PF_R | PF_W | PF_X)) == (PF_R | PF_X)) {
				// TODO: share this segment
			}else{
				debug::panicLogger.log() << "Illegal combination of segment permissions"
						<< debug::Finish();
			}

			ASSERT(phdr->p_memsz > 0);
			
			// align virtual address and length to page size
			uintptr_t virt_address = phdr->p_vaddr;
			virt_address -= virt_address % kPageSize;

			size_t virt_length = (phdr->p_vaddr + phdr->p_memsz) - virt_address;
			if((virt_length % kPageSize) != 0)
				virt_length += kPageSize - virt_length % kPageSize;

			object->segments.push(UniqueSegment(phdr->p_type,
					phdr->p_flags, virt_address, virt_length,
					phdr->p_vaddr - virt_address, phdr->p_offset, phdr->p_filesz));
		}else if(phdr->p_type == PT_DYNAMIC) {
			object->dynamic = phdr->p_vaddr;
		} //FIXME: handle other phdrs
	}
	
	return object;
}

void sendObject(HelHandle pipe, int64_t request_id,
		Object *object, uintptr_t base_address) {
	protobuf::FixedWriter<128> object_writer;
	protobuf::emitUInt64(object_writer,
			managarm::ld_server::ServerResponse::kField_entry,
			base_address + object->entry);
	protobuf::emitUInt64(object_writer,
			managarm::ld_server::ServerResponse::kField_dynamic,
			base_address + object->dynamic);

	for(size_t i = 0; i < object->segments.size(); i++) {
		Segment &wrapper = object->segments[i];
		
		BaseSegment *base_segment;
		HelHandle memory;
		if(wrapper.is<SharedSegment>()) {
			auto &segment = wrapper.get<SharedSegment>();
			base_segment = &segment;
			
			memory = segment.memory;
		}else{
			ASSERT(wrapper.is<UniqueSegment>());
			auto &segment = wrapper.get<UniqueSegment>();
			base_segment = &segment;

			helAllocateMemory(segment.virtLength, &memory);

			void *map_pointer;
			helMapMemory(memory, kHelNullHandle, nullptr,
					segment.virtLength, kHelMapReadWrite, &map_pointer);
			memset(map_pointer, 0, segment.virtLength);
			memcpy((void *)((uintptr_t)map_pointer + segment.fileDisplacement),
					(void *)((uintptr_t)object->imagePtr + segment.fileOffset),
					segment.fileLength);
		}
		
		protobuf::FixedWriter<16> segment_writer;
		protobuf::emitUInt64(segment_writer,
				managarm::ld_server::Segment::kField_virt_address,
				base_address + base_segment->virtAddress);
		protobuf::emitUInt64(segment_writer,
				managarm::ld_server::Segment::kField_virt_length,
				base_segment->virtLength);

		if((base_segment->elfFlags & (PF_R | PF_W | PF_X)) == (PF_R | PF_W)) {
			protobuf::emitInt32(segment_writer,
					managarm::ld_server::Segment::kField_access,
					managarm::ld_server::Access::READ_WRITE);
		}else if((base_segment->elfFlags & (PF_R | PF_W | PF_X)) == (PF_R | PF_X)) {
			protobuf::emitInt32(segment_writer,
					managarm::ld_server::Segment::kField_access,
					managarm::ld_server::Access::READ_EXECUTE);
		}else{
			debug::panicLogger.log() << "Illegal combination of segment permissions"
					<< debug::Finish();
		}
		
		protobuf::emitMessage(object_writer,
				managarm::ld_server::ServerResponse::kField_segments,
				segment_writer);
		
		helSendDescriptor(pipe, memory, 1, 1 + i);
	}

	helSendString(pipe,
			object_writer.data(), object_writer.size(), 1, 0);
}

util::LazyInitializer<helx::EventHub> eventHub;
util::LazyInitializer<helx::Server> server;

struct ProcessContext {
	ProcessContext(HelHandle pipe_handle)
	: pipeHandle(pipe_handle) { }

	HelHandle pipeHandle;
	uint8_t buffer[128];
};

auto processRequests =
async::repeatWhile(
	async::lambda([](ProcessContext &context, util::Callback<void(bool)> callback) {
		callback(true);
	}),
	async::seq(
		async::lambda([](ProcessContext &context,
				util::Callback<void(HelError, int64_t, int64_t, size_t)> callback) {
			int64_t async_id;
			helSubmitRecvString(context.pipeHandle, eventHub->getHandle(),
					context.buffer, 128, kHelAnyRequest, 0,
					(uintptr_t)callback.getFunction(),
					(uintptr_t)callback.getObject(),
					&async_id);
		}),
		async::lambda([](ProcessContext &context, util::Callback<void()> callback,
				HelError error, int64_t msg_request, int64_t msg_sequence, size_t length) {
			char ident_buffer[64];
			size_t ident_length = 0;
			uint64_t base_address = 0;

			protobuf::BufferReader reader(context.buffer, length);
			while(!reader.atEnd()) {
				auto header = protobuf::fetchHeader(reader);
				switch(header.field) {
				case managarm::ld_server::ClientRequest::kField_identifier:
					ident_length = protobuf::fetchString(reader, ident_buffer, 64);
					break;
				case managarm::ld_server::ClientRequest::kField_base_address:
					base_address = protobuf::fetchUInt64(reader);
					break;
				default:
					ASSERT(!"Unexpected field in ClientRequest");
				}
			}
			
			Object *object = readObject(util::StringView(ident_buffer, ident_length));
			sendObject(context.pipeHandle, msg_request, object, base_address);
			callback();
		})
	)
);

void onAccept(void *object, HelError error, HelHandle pipe_handle) {
	async::run(*allocator, processRequests, ProcessContext(pipe_handle),
		[](ProcessContext &context) { });
	
	server->accept(*eventHub, nullptr, &onAccept);
}

int main() {
	infoLogger.initialize(infoSink);
	infoLogger->log() << "Entering ld-server" << debug::Finish();
	allocator.initialize(virtualAlloc);

	eventHub.initialize();

	// create a server and listen for requests
	HelHandle serve_handle, client_handle;
	helCreateServer(&serve_handle, &client_handle);

	server.initialize(serve_handle);
	server->accept(*eventHub, nullptr, &onAccept);
	
	// inform k_init that we are ready to server requests
	const char *path = "k_init";
	HelHandle parent_handle;
	helRdOpen(path, strlen(path), &parent_handle);

	helx::Pipe parent_pipe(parent_handle);
	parent_pipe.sendDescriptor(client_handle, 1, 0);

	infoLogger->log() << "ld-server initialized succesfully!" << debug::Finish();

	while(true)
		eventHub->defaultProcessEvents();
	
	return 0;
}

