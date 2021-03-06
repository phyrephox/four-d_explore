// Imports.
#include "app.h"
#include <cmath>
#include <fstream>
#include <functional>
#include <iostream>
#include <string>
#include "config.h"
#include "misc/glsl_to_spirv.h"
#include "misc/io.h"
#include "misc/memory_allocator.h"
#include "misc/object_tracker.h"
#include "misc/time.h"
#include "misc/window_factory.h"
#include "wrappers/buffer.h"
#include "wrappers/command_buffer.h"
#include "wrappers/command_pool.h"
#include "wrappers/descriptor_set_group.h"
#include "wrappers/descriptor_set_layout.h"
#include "wrappers/device.h"
#include "wrappers/event.h"
#include "wrappers/compute_pipeline_manager.h"
#include "wrappers/graphics_pipeline_manager.h"
#include "wrappers/framebuffer.h"
#include "wrappers/image.h"
#include "wrappers/image_view.h"
#include "wrappers/instance.h"
#include "wrappers/physical_device.h"
#include "wrappers/rendering_surface.h"
#include "wrappers/query_pool.h"
#include "wrappers/render_pass.h"
#include "wrappers/semaphore.h"
#include "wrappers/shader_module.h"
#include "wrappers/swapchain.h"
#include "vulkan/vulkan.h"
#include "matrix.h"
#include "callback.h"
#include "glm/gtc/matrix_transform.hpp"
#ifdef _WIN32
#define GLFW_EXPOSE_NATIVE_WIN32
#else
#define GLFW_EXPOSE_NATIVE_X11
#include <X11/Xlib-xcb.h>
#endif
#include "GLFW/glfw3native.h"

// Debug flags.
// #define ENABLE_VALIDATION

// Constants.
#define APP_NAME "Four Dimensional Exploration"
#define DEBUG_REREAD 0
#define DEBUG_FRAME_TIME 0
#define DEBUG_BAKE_TIME 1

/*
 *	Create the app and assign default values to several field variables.
 */
App::App(int width, int height, std::vector<Terrain::Block*> blocks)
	: windowWidth_(width),
	windowHeight_(height),
	blocks_(blocks),
	n_last_semaphore_used_(0),
	n_swapchain_images_(N_SWAPCHAIN_IMAGES),
	prev_time(std::chrono::steady_clock::now()) {
}

/*
 This function initializes the app through a series of smaller initialization
 steps.
 The GPUOpen example project "PushConstants" was a starting point for this
 project.
 https://github.com/GPUOpen-LibrariesAndSDKs/Anvil/blob/master/examples/PushConstants
 */
void App::init() {
	init_meshes();
	init_vulkan();
	init_window();
	init_swapchain();
	printf("s1\n");
	init_buffers();
	printf("s2\n");
	init_dsgs();
	printf("s3\n");
	init_images();
	printf("s4\n");
	init_semaphores();
	printf("s5\n");
	init_shaders();
	printf("s6\n");

	init_compute_pipelines();
	printf("s7\n");
	init_framebuffers();
	printf("s8\n");
	init_gfx_pipelines();
	printf("s9\n");
	init_command_buffers();

	printf("s10\n");
	init_camera();
}

/*
 *	MESH INITIALIZATION.
 *	Initialize the vector of tesseract centers given the input terrain data.
 */

 // Prepare the actual values of input mesh data.
 // Some preconfigured testing scenes are available in shapes.h.
std::vector<glm::vec4> MESH_CENTERS;

// Set to 64 for wire mesh, 144 for closed figure.
int N_VERTICES = 144;
int N_MESHES = 0;

void App::init_meshes() {
	for (int i = 0; i < blocks_.size(); i++) {
		Terrain::Block* block = blocks_.at(i);
		if (block->GetType() > 0) {
			MESH_CENTERS.push_back(block->GetPos());
		}
	}
	N_MESHES = MESH_CENTERS.size();
}

/*
  VULKAN INITIALIZATION.
  Initialize the Vulkan context to work with this app.
 */
void App::init_vulkan() {
	instance_ptr_ = Anvil::Instance::create(APP_NAME, APP_NAME,
#ifdef ENABLE_VALIDATION
		on_validation_callback,
#else
		nullptr,
#endif
		nullptr);
	physical_device_ptr_ = instance_ptr_->get_physical_device(0);
	device_ptr_ = Anvil::SGPUDevice::create(
		physical_device_ptr_, Anvil::DeviceExtensionConfiguration(),
		std::vector<std::string>(), false, false);
}

/*
  WINDOW INITIALIZATION.
  Initialize the window for displaying this app.
 */
void App::init_window() {
	InitializeWindow(windowWidth_, windowHeight_, APP_NAME);

#ifdef _WIN32
	const Anvil::WindowPlatform platform = Anvil::WINDOW_PLATFORM_SYSTEM;
	WindowHandle handle = glfwGetWin32Window(GetGLFWWindow());
	void* xcb_ptr = nullptr;
#else
	const Anvil::WindowPlatform platform = Anvil::WINDOW_PLATFORM_XCB;
	WindowHandle handle = glfwGetX11Window(GetGLFWWindow());
	void* xcb_ptr = (void*)XGetXCBConnection(glfwGetX11Display());
#endif

	window_ptr_ = Anvil::WindowFactory::create_window(platform, handle, xcb_ptr);
}

/*
  SWAPCHAIN INITIALIZATION.
  Initialize the app's main swapchain.
 */
void App::init_swapchain() {
	std::shared_ptr<Anvil::SGPUDevice> device_locked_ptr(device_ptr_);
	rendering_surface_ptr_ =
		Anvil::RenderingSurface::create(instance_ptr_, device_ptr_, window_ptr_);
	//rendering_surface_ptr_->get_surface_ptr() = &surface_;

	rendering_surface_ptr_->set_name("Main rendering surface");

	swapchain_ptr_ = device_locked_ptr->create_swapchain(
		rendering_surface_ptr_, window_ptr_, VK_FORMAT_B8G8R8A8_UNORM,
		VK_PRESENT_MODE_FIFO_KHR, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
		n_swapchain_images_);
	swapchain_ptr_->set_name("Main swapchain");

	/* Cache the queue we are going to use for presentation */
	const std::vector<uint32_t>* present_queue_fams_ptr = nullptr;
	if (!rendering_surface_ptr_->get_queue_families_with_present_support(
		device_locked_ptr->get_physical_device(), &present_queue_fams_ptr)) {
		anvil_assert_fail();
	}

	present_queue_ptr_ =
		device_locked_ptr->get_queue(present_queue_fams_ptr->at(0), 0);
}

/*
 BUFFER INITIALIZATION.
 Initialize the buffers for geometry in the scene.
 This section includes relevant helper functions.
 */

 // Buffer initialization.
void App::init_buffers() {
	// Setup the memory allocator to begin initializing data buffers.
	std::shared_ptr<Anvil::MemoryAllocator> memory_allocator_ptr;
	std::shared_ptr<Anvil::PhysicalDevice> physical_device_locked_ptr(
		physical_device_ptr_);
	const VkDeviceSize sb_data_alignment_requirement =
		physical_device_locked_ptr->get_device_properties()
		.limits.minStorageBufferOffsetAlignment;
	memory_allocator_ptr = Anvil::MemoryAllocator::create_oneshot(device_ptr_);

	// NEW: cube.
	// Figure out what size is needed for the input buffer of cube vertices.
	totalInputCubeBufferSize_ = 0;
	for (uint32_t vertexIndex = 0; vertexIndex < N_MESHES; ++vertexIndex) {
		// Store current data offset.
		anvil_assert((totalInputCubeBufferSize_ % sb_data_alignment_requirement) ==
			0);

		// Tim's platform has different offsets?
#ifdef _WIN32
		inputCubeElementOffsets_.push_back(totalInputCubeBufferSize_ / 2);
#else
		inputCubeElementOffsets_.push_back(totalInputCubeBufferSize_);
#endif

		// Account for space necessary to hold a vec4 and any padding required to
		// meet the alignment requirement.
		totalInputCubeBufferSize_ += (sizeof(float) * 4);
		totalInputCubeBufferSize_ +=
			(sb_data_alignment_requirement -
				totalInputCubeBufferSize_ % sb_data_alignment_requirement) %
			sb_data_alignment_requirement;
	}

	// Create the layout buffer for storing the input cube vertices.
	inputCubeBufferPointer_ = Anvil::Buffer::create_nonsparse(
		device_ptr_, totalInputCubeBufferSize_,
		Anvil::QUEUE_FAMILY_COMPUTE_BIT | Anvil::QUEUE_FAMILY_GRAPHICS_BIT,
		VK_SHARING_MODE_CONCURRENT, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
	inputCubeBufferPointer_->set_name("Cube input vertices");
	memory_allocator_ptr->add_buffer(inputCubeBufferPointer_, 0);

	std::unique_ptr<char> inputCubeBufferValues;
	inputCubeBufferValues.reset(
		new char[static_cast<uintptr_t>(totalInputCubeBufferSize_)]);
	for (uint32_t vertexIndex = 0; vertexIndex < N_MESHES; ++vertexIndex) {
		float* cubeVertexDataPointer =
			(float*)(inputCubeBufferValues.get() +
				inputCubeElementOffsets_[vertexIndex]);

		// Populate the component coordinates for each input vertex.
		glm::vec4 vertex = MESH_CENTERS[vertexIndex];
		*cubeVertexDataPointer = vertex.x;
		cubeVertexDataPointer++;
		*cubeVertexDataPointer = vertex.y;
		cubeVertexDataPointer++;
		*cubeVertexDataPointer = vertex.z;
		cubeVertexDataPointer++;
		*cubeVertexDataPointer = vertex.w;
	}

	// Now prepare a memory block which is going to hold vertex data generated by
	// the compute shader:
	outputCubeVerticesBufferSize_ = 0;
	for (unsigned int vertexIndex = 0; vertexIndex < (N_MESHES * N_VERTICES); ++vertexIndex) {
		// Store current offset and account for space necessary to hold it.
		// Tim's platform has different offsets?
#ifdef _WIN32
		outputCubeVerticesBufferSizes_.push_back(outputCubeVerticesBufferSize_ / 2);
#else
		outputCubeVerticesBufferSizes_.push_back(outputCubeVerticesBufferSize_);
#endif

		outputCubeVerticesBufferSize_ += (sizeof(float) * 4);

		// Account for space necessary to hold a vec4 for each point
		// and any padding required to meet the alignment requirement.
		outputCubeVerticesBufferSize_ +=
			(sb_data_alignment_requirement -
				outputCubeVerticesBufferSize_ % sb_data_alignment_requirement) %
			sb_data_alignment_requirement;
		anvil_assert(
			outputCubeVerticesBufferSize_ % sb_data_alignment_requirement == 0);
	}

	// Allocate the memory for the buffer of output vertices.
	outputCubeVerticesBufferPointer_ = Anvil::Buffer::create_nonsparse(
		device_ptr_, outputCubeVerticesBufferSize_,
		Anvil::QUEUE_FAMILY_COMPUTE_BIT | Anvil::QUEUE_FAMILY_GRAPHICS_BIT,
		VK_SHARING_MODE_CONCURRENT, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
	outputCubeVerticesBufferPointer_->set_name("Cube output vertices");
	memory_allocator_ptr->add_buffer(outputCubeVerticesBufferPointer_, 0);

	// Find size for sroting the 4D view matrix.
	const auto dynamic_ub_alignment_requirement =
		device_ptr_.lock()
		->get_physical_device_properties()
		.limits.minUniformBufferOffsetAlignment;
	const auto localMat5UniformSizePerSwapchain = Anvil::Utils::round_up(
		sizeof(glm::mat4) + 2 * sizeof(glm::vec4) + sizeof(float),
		dynamic_ub_alignment_requirement);
	const auto mat5_data_buffer_size_total =
		localMat5UniformSizePerSwapchain * N_SWAPCHAIN_IMAGES;
	mat5UniformSizePerSwapchain = localMat5UniformSizePerSwapchain;

	// Create the layout buffer for storing viewProj in the compute shader.
	viewProjUniformPointer = Anvil::Buffer::create_nonsparse(
		device_ptr_, mat5_data_buffer_size_total,
		Anvil::QUEUE_FAMILY_COMPUTE_BIT | Anvil::QUEUE_FAMILY_GRAPHICS_BIT,
		VK_SHARING_MODE_CONCURRENT, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT);
	viewProjUniformPointer->set_name("View Proj data buffer");
	memory_allocator_ptr->add_buffer(viewProjUniformPointer,
		Anvil::MEMORY_FEATURE_FLAG_MAPPABLE);

	// Create the layout buffer for storing viewMatrix in the compute shader.
	viewMatrixUniformPointer = Anvil::Buffer::create_nonsparse(
		device_ptr_, mat5_data_buffer_size_total,
		Anvil::QUEUE_FAMILY_COMPUTE_BIT | Anvil::QUEUE_FAMILY_GRAPHICS_BIT,
		VK_SHARING_MODE_CONCURRENT, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT);
	viewMatrixUniformPointer->set_name("View Matrix data buffer");
	memory_allocator_ptr->add_buffer(viewMatrixUniformPointer,
		Anvil::MEMORY_FEATURE_FLAG_MAPPABLE);

	// Assign memory blocks to cube input vertices buffer and fill with values.
	inputCubeBufferPointer_->write(0, inputCubeBufferPointer_->get_size(),
		inputCubeBufferValues.get());
}

/*
  DESCRIPTOR SET GROUP INITIALIZATION.
  Creates a descriptor set group, binding uniform data buffers.
 */
void App::init_dsgs() {
	/* Create the descriptor set layouts for the generator program. */
	compute_dsg_ptr_ = Anvil::DescriptorSetGroup::create(
		device_ptr_, false, /* releaseable_sets */
		2 /* n_sets           */);

	compute_dsg_ptr_->add_binding(0, /* n_set      */
		0, /* binding    */
		VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
		1, /* n_elements */
		VK_SHADER_STAGE_COMPUTE_BIT);

	printf("dsg1\n");
	compute_dsg_ptr_->add_binding(1,  // Set.
		0,  // Binding.
		VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
		1,  // n elements.
		VK_SHADER_STAGE_COMPUTE_BIT);

	printf("dsg3\n");
	compute_dsg_ptr_->add_binding(1,  // Set.
		1,  // Binding.
		VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
		1,  // n elements.
		VK_SHADER_STAGE_COMPUTE_BIT);

	// Bind to the compute shader a uniform layout for storing the current time.
	compute_dsg_ptr_->set_binding_item(
		0,  // Set.
		0,  // Binding.
		Anvil::DescriptorSet::UniformBufferBindingElement(
			viewProjUniformPointer,
			0,  // Offset.
			mat5UniformSizePerSwapchain));

	printf("dsg2\n");
	// NEW: cube
	// Bind to the compute shader a buffer for recording input cube vertices.
	compute_dsg_ptr_->set_binding_item(
		1,  // Set.
		0,  // Binding.
		Anvil::DescriptorSet::StorageBufferBindingElement(
			inputCubeBufferPointer_,
			0,  // Offset.
			sizeof(float) * 4 * N_MESHES));

	printf("dsg4\n");
	// Bind to the compute shader a buffer for recording the output cube vertices.
	compute_dsg_ptr_->set_binding_item(
		1,  // Set.
		1,  // Binding.
		Anvil::DescriptorSet::StorageBufferBindingElement(
			outputCubeVerticesBufferPointer_,
			0,  // Offset.
			sizeof(float) * 4 * N_MESHES * N_VERTICES));
	printf("dsg5\n");

	/* Set up the descriptor set layout for the renderer program.  */
	dsg_ptr_ = Anvil::DescriptorSetGroup::create(device_ptr_,
		false, /* releaseable_sets */
		1 /* n_sets           */);

	dsg_ptr_->add_binding(0,                                    /* n_set      */
		0,                                    /* binding    */
		VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, /* n_elements */
		VK_SHADER_STAGE_VERTEX_BIT);

	dsg_ptr_->set_binding_item(
		0, /* n_set         */
		0, /* binding_index */
		Anvil::DescriptorSet::StorageBufferBindingElement(
			outputCubeVerticesBufferPointer_, 0, /* in_start_offset */
			sizeof(float) * 4 * N_MESHES * N_VERTICES));

	axis_dsg_ptr_ = Anvil::DescriptorSetGroup::create(device_ptr_, false, 1);
	axis_dsg_ptr_->add_binding(0, 0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1,
		VK_SHADER_STAGE_VERTEX_BIT);

	axis_dsg_ptr_->set_binding_item(
		0, 0, Anvil::DescriptorSet::UniformBufferBindingElement(
			viewMatrixUniformPointer, 0,
			mat5UniformSizePerSwapchain));
}

/*
  FRAME BUFFER INITIALIZATION.
  This function creates, for every image in the swapchain, a series of
  most-optimal
  tasks at each step.
 */
void App::init_framebuffers() {
	// Instantiate a framebuffer object for each swapchain image.
	for (uint32_t n_fbo = 0; n_fbo < N_SWAPCHAIN_IMAGES; ++n_fbo) {
		// Retrieve a pointer to the image.
		std::shared_ptr<Anvil::ImageView> attachment_image_view_ptr;
		attachment_image_view_ptr = swapchain_ptr_->get_image_view(n_fbo);

		// Create a framebuffer entry for this one-layer image.
		fbos_[n_fbo] =
			Anvil::Framebuffer::create(device_ptr_, windowWidth_, windowHeight_, 1);
		fbos_[n_fbo]->set_name_formatted("Framebuffer for swapchain image [%d]",
			n_fbo);

		// Attach this view to the growing list.
		bool result = fbos_[n_fbo]->add_attachment(attachment_image_view_ptr,
			nullptr); /* out_opt_attachment_id_ptr */
		anvil_assert(result);

		result = fbos_[n_fbo]->add_attachment(depth_image_views_[n_fbo],
			nullptr); /* out_opt_attachment_id_ptr */
	}
}

/*
  SEMAPHORE INITIALIZATION.
  Initialize the sempahores that we use here to ensure proper order and
  correctness.
 */
void App::init_semaphores() {
	// Iterate through all associated semaphores.
	for (uint32_t n_semaphore = 0; n_semaphore < n_swapchain_images_;
	++n_semaphore) {
		// Retrieve pointers to the semaphore grabs.
		std::shared_ptr<Anvil::Semaphore> new_signal_semaphore_ptr =
			Anvil::Semaphore::create(device_ptr_);
		std::shared_ptr<Anvil::Semaphore> new_wait_semaphore_ptr =
			Anvil::Semaphore::create(device_ptr_);

		// Display semaphore update information.
		new_signal_semaphore_ptr->set_name_formatted("Signal semaphore [%d]",
			n_semaphore);
		new_wait_semaphore_ptr->set_name_formatted("Wait semaphore [%d]",
			n_semaphore);

		// Push new semaphore data.
		frame_signal_semaphores_.push_back(new_signal_semaphore_ptr);
		frame_wait_semaphores_.push_back(new_wait_semaphore_ptr);
	}
}

// Display the interesting output of the shaders!
void App::init_shaders() {
	// Read the compute shader from a separate file.
	// If running on Windows, assume it's Tim's machine and resolve this hacky path
	// garbage.
#ifdef _WIN32
	std::ifstream infileComp{
		"E:\\Penn 17 - 18\\CIS 565\\four-d_explore\\src\\shaders\\example.comp" };
#else
	std::ifstream infileComp{ "../src/shaders/example.comp" };
#endif
	std::string compute{ std::istreambuf_iterator<char>(infileComp),
						std::istreambuf_iterator<char>() };

	// Read the vertex shader from a separate file.
	// If running on Windows, assume it's Tim's machine and resolve this hacky path
	// garbage.
#ifdef _WIN32
	std::ifstream infileVertex{
		"E:\\Penn 17 - 18\\CIS 565\\four-d_explore\\src\\shaders\\example.vert" };
#else
	std::ifstream infileVertex{ "../src/shaders/example.vert" };
#endif
	std::string vertex{ std::istreambuf_iterator<char>(infileVertex),
					   std::istreambuf_iterator<char>() };

	// Read the vertex shader from a separate file.
	// If running on Windows, assume it's Tim's machine and resolve this hacky path
	// garbage.
#ifdef _WIN32
	std::ifstream infileVertex2{
		"E:\\Penn 17 - 18\\CIS 565\\four-d_explore\\src\\shaders\\axes.vert" };
#else
	std::ifstream infileVertex2{ "../src/shaders/axes.vert" };
#endif
	std::string vertex2{ std::istreambuf_iterator<char>(infileVertex2),
						std::istreambuf_iterator<char>() };

	// Read the fragment shader from a separate file.
	// If running on Windows, assume it's Tim's machine and resolve this hacky path
	// garbage.
#ifdef _WIN32
	std::ifstream infileFragment{
		"E:\\Penn 17 - 18\\CIS 565\\four-d_explore\\src\\shaders\\example.frag" };
#else
	std::ifstream infileFragment{ "../src/shaders/example.frag" };
#endif
	std::string fragment{ std::istreambuf_iterator<char>(infileFragment),
						 std::istreambuf_iterator<char>() };

	// Read the geometry shader from a separate file.
	// If running on Windows, assume it's Tim's machine and resolve this hacky path
	// garbage.
	std::string file_name = "";
	if (N_VERTICES == 144) {
		file_name = "tri.geom";
	} else {
		file_name = "line.geom";
	}
#ifdef _WIN32
	std::ifstream infileGeometry{
		"E:\\Penn 17 - 18\\CIS 565\\four-d_explore\\src\\shaders\\" + file_name };
#else
	std::ifstream infileGeometry{ "../src/shaders/" + file_name };
#endif
	std::string geo{ std::istreambuf_iterator<char>(infileGeometry),
					std::istreambuf_iterator<char>() };

	std::shared_ptr<Anvil::GLSLShaderToSPIRVGenerator> compute_shader_ptr;
	std::shared_ptr<Anvil::ShaderModule> compute_shader_module_ptr;
	std::shared_ptr<Anvil::GLSLShaderToSPIRVGenerator> fragment_shader_ptr;
	std::shared_ptr<Anvil::ShaderModule> fragment_shader_module_ptr;
	std::shared_ptr<Anvil::GLSLShaderToSPIRVGenerator> vertex_shader_ptr;
	std::shared_ptr<Anvil::ShaderModule> vertex_shader_module_ptr;
	std::shared_ptr<Anvil::GLSLShaderToSPIRVGenerator> axis_shader_ptr;
	std::shared_ptr<Anvil::ShaderModule> axis_shader_module_ptr;
	std::shared_ptr<Anvil::GLSLShaderToSPIRVGenerator> geo_shader_ptr;
	std::shared_ptr<Anvil::ShaderModule> geo_shader_module_ptr;

	compute_shader_ptr = Anvil::GLSLShaderToSPIRVGenerator::create(
		device_ptr_, Anvil::GLSLShaderToSPIRVGenerator::MODE_USE_SPECIFIED_SOURCE,
		compute, Anvil::SHADER_STAGE_COMPUTE);
	vertex_shader_ptr = Anvil::GLSLShaderToSPIRVGenerator::create(
		device_ptr_, Anvil::GLSLShaderToSPIRVGenerator::MODE_USE_SPECIFIED_SOURCE,
		vertex, Anvil::SHADER_STAGE_VERTEX);
	axis_shader_ptr = Anvil::GLSLShaderToSPIRVGenerator::create(
		device_ptr_, Anvil::GLSLShaderToSPIRVGenerator::MODE_USE_SPECIFIED_SOURCE,
		vertex2, Anvil::SHADER_STAGE_VERTEX);
	fragment_shader_ptr = Anvil::GLSLShaderToSPIRVGenerator::create(
		device_ptr_, Anvil::GLSLShaderToSPIRVGenerator::MODE_USE_SPECIFIED_SOURCE,
		fragment, Anvil::SHADER_STAGE_FRAGMENT);
	geo_shader_ptr = Anvil::GLSLShaderToSPIRVGenerator::create(
		device_ptr_, Anvil::GLSLShaderToSPIRVGenerator::MODE_USE_SPECIFIED_SOURCE,
		geo, Anvil::SHADER_STAGE_GEOMETRY);

	/* Set up GLSLShader instances */
	printf("Attempting to transmit N_MESHES: %d\n", N_MESHES);
	compute_shader_ptr->add_definition_value_pair("N_MESHES", N_MESHES);
	vertex_shader_ptr->add_definition_value_pair("N_MESHES", N_MESHES);
	compute_shader_ptr->add_definition_value_pair("N_VERTICES", N_VERTICES);
	vertex_shader_ptr->add_definition_value_pair("N_VERTICES", N_VERTICES);

	compute_shader_module_ptr = Anvil::ShaderModule::create_from_spirv_generator(
		device_ptr_, compute_shader_ptr);
	fragment_shader_module_ptr = Anvil::ShaderModule::create_from_spirv_generator(
		device_ptr_, fragment_shader_ptr);
	vertex_shader_module_ptr = Anvil::ShaderModule::create_from_spirv_generator(
		device_ptr_, vertex_shader_ptr);
	axis_shader_module_ptr = Anvil::ShaderModule::create_from_spirv_generator(
		device_ptr_, axis_shader_ptr);
	geo_shader_module_ptr = Anvil::ShaderModule::create_from_spirv_generator(
		device_ptr_, geo_shader_ptr);

	compute_shader_module_ptr->set_name("Compute shader module");
	fragment_shader_module_ptr->set_name("Fragment shader module");
	vertex_shader_module_ptr->set_name("Vertex shader module");
	axis_shader_module_ptr->set_name("Axis shader module");
	geo_shader_module_ptr->set_name("Geometry shader module");

	cs_ptr_.reset(new Anvil::ShaderModuleStageEntryPoint(
		"main", compute_shader_module_ptr, Anvil::SHADER_STAGE_COMPUTE));
	fs_ptr_.reset(new Anvil::ShaderModuleStageEntryPoint(
		"main", fragment_shader_module_ptr, Anvil::SHADER_STAGE_FRAGMENT));
	vs_ptr_.reset(new Anvil::ShaderModuleStageEntryPoint(
		"main", vertex_shader_module_ptr, Anvil::SHADER_STAGE_VERTEX));
	vs_axis_ptr_.reset(new Anvil::ShaderModuleStageEntryPoint(
		"main", axis_shader_module_ptr, Anvil::SHADER_STAGE_VERTEX));
	ge_ptr_.reset(new Anvil::ShaderModuleStageEntryPoint(
		"main", geo_shader_module_ptr, Anvil::SHADER_STAGE_GEOMETRY));
}

/*
  COMPUTE PIPELINE INITIALIZATION.
  Link and setup the several stages of this application with compute steps.
 */
void App::init_compute_pipelines() {
	std::shared_ptr<Anvil::SGPUDevice> device_locked_ptr(device_ptr_);
	std::shared_ptr<Anvil::ComputePipelineManager> compute_manager_ptr(
		device_locked_ptr->get_compute_pipeline_manager());
	bool result;

	/* Create & configure the compute pipeline */
	result = compute_manager_ptr->add_regular_pipeline(
		false, /* disable_optimizations */
		false, /* allow_derivatives     */
		*cs_ptr_, &compute_pipeline_id_);
	anvil_assert(result);
	printf("ic1\n");

	result = compute_manager_ptr->set_pipeline_dsg(compute_pipeline_id_,
		compute_dsg_ptr_);
	anvil_assert(result);

	printf("Baking meshes...\n");
	if (!DEBUG_FRAME_TIME && DEBUG_BAKE_TIME) {
		prev_time = std::chrono::steady_clock::now();
	}
	result = compute_manager_ptr->bake();
	anvil_assert(result);
	if (!DEBUG_FRAME_TIME && DEBUG_BAKE_TIME) {
		auto cur_time = std::chrono::steady_clock::now();
		std::chrono::duration<double, std::milli> dif = cur_time - prev_time;
		std::cout << "Baked in " << dif.count() << "ms.\n";
		prev_time = cur_time;
	}
}

/*
  GRAPHICS PIPELINE INITIALIZATION.
  Link together important steps needed for rendering in phases--the pipeline
  steps.
 */
void App::init_gfx_pipelines() {
	std::shared_ptr<Anvil::SGPUDevice> device_locked_ptr(device_ptr_);
	std::shared_ptr<Anvil::GraphicsPipelineManager> gfx_manager_ptr(
		device_locked_ptr->get_graphics_pipeline_manager());
	bool result;

	/* Create a renderpass instance */
#ifdef ENABLE_OFFSCREEN_RENDERING
	const VkImageLayout final_layout = VK_IMAGE_LAYOUT_GENERAL;
#else
	const VkImageLayout final_layout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
#endif

	Anvil::RenderPassAttachmentID render_pass_color_attachment_id = -1;
	Anvil::RenderPassAttachmentID render_pass_depth_attachment_id = -1;
	Anvil::SubPassID render_pass_subpass_id = -1;
	Anvil::RenderPassAttachmentID axis_render_pass_color_attachment_id = -1;
	Anvil::RenderPassAttachmentID axis_render_pass_depth_attachment_id = -1;
	Anvil::SubPassID axis_render_pass_subpass_id = -1;

	renderpass_ptr_ =
		Anvil::RenderPass::create(device_ptr_, swapchain_ptr_);
	axis_renderpass_ptr_ =
		Anvil::RenderPass::create(device_ptr_, swapchain_ptr_);

	renderpass_ptr_->set_name("Consumer renderpass");
	axis_renderpass_ptr_->set_name("Axis renderpass");

	result = renderpass_ptr_->add_color_attachment(
		swapchain_ptr_->get_image_format(), VK_SAMPLE_COUNT_1_BIT,
		VK_ATTACHMENT_LOAD_OP_CLEAR, VK_ATTACHMENT_STORE_OP_STORE,
		VK_IMAGE_LAYOUT_UNDEFINED, final_layout, false, /* may_alias */
		&render_pass_color_attachment_id);
	anvil_assert(result);

	result = axis_renderpass_ptr_->add_color_attachment(
		swapchain_ptr_->get_image_format(), VK_SAMPLE_COUNT_1_BIT,
		VK_ATTACHMENT_LOAD_OP_LOAD, VK_ATTACHMENT_STORE_OP_STORE,
		VK_IMAGE_LAYOUT_UNDEFINED, final_layout, false, /* may_alias */
		&axis_render_pass_color_attachment_id);
	anvil_assert(result);

	result = renderpass_ptr_->add_depth_stencil_attachment(
		depth_images_[0]->get_image_format(),
		depth_images_[0]->get_image_sample_count(),
		VK_ATTACHMENT_LOAD_OP_CLEAR,                      /* depth_load_op    */
		VK_ATTACHMENT_STORE_OP_DONT_CARE,                 /* depth_store_op   */
		VK_ATTACHMENT_LOAD_OP_DONT_CARE,                  /* stencil_load_op  */
		VK_ATTACHMENT_STORE_OP_DONT_CARE,                 /* stencil_store_op */
		VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL, /* initial_layout   */
		VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL, /* final_layout     */
		false,                                            /* may_alias        */
		&render_pass_depth_attachment_id);
	anvil_assert(result);

	result = renderpass_ptr_->add_subpass(
		*fs_ptr_, *ge_ptr_,                   /* geometry_shader */
		//Anvil::ShaderModuleStageEntryPoint(),
		Anvil::ShaderModuleStageEntryPoint(), /* tess_control_shader    */
		Anvil::ShaderModuleStageEntryPoint(), /* tess_evaluation_shader */
		*vs_ptr_, &render_pass_subpass_id);
	anvil_assert(result);
	result = axis_renderpass_ptr_->add_subpass(
		*fs_ptr_,
		Anvil::ShaderModuleStageEntryPoint(),
		Anvil::ShaderModuleStageEntryPoint(),
		Anvil::ShaderModuleStageEntryPoint(),
		*vs_axis_ptr_, &axis_render_pass_subpass_id);
	anvil_assert(result);

	result = renderpass_ptr_->add_subpass_color_attachment(
		render_pass_subpass_id, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
		render_pass_color_attachment_id, 0, /* location                      */
		nullptr);                           /* opt_attachment_resolve_id_ptr */
	result &= renderpass_ptr_->add_subpass_depth_stencil_attachment(
		render_pass_subpass_id, render_pass_depth_attachment_id,
		VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);
	anvil_assert(result);

	result = axis_renderpass_ptr_->add_subpass_color_attachment(
		axis_render_pass_subpass_id, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
		axis_render_pass_color_attachment_id, 0, /* location */
		nullptr); /* opt_attachment_resolve_id_ptr */
	anvil_assert(result);

	/* Set up the graphics pipeline for the main subpass */
	result = renderpass_ptr_->get_subpass_graphics_pipeline_id(
		render_pass_subpass_id, &pipeline_id_);
	anvil_assert(result);

	result = axis_renderpass_ptr_->get_subpass_graphics_pipeline_id(
		axis_render_pass_subpass_id, &axis_pipeline_id_);
	anvil_assert(result);

	gfx_manager_ptr->add_vertex_attribute(pipeline_id_, 0, /* location */
		VK_FORMAT_R32G32B32A32_SFLOAT,
		0,                 /* offset_in_bytes */
		sizeof(float) * 1, /* stride_in_bytes */
		VK_VERTEX_INPUT_RATE_INSTANCE);
	gfx_manager_ptr->set_pipeline_dsg(pipeline_id_, dsg_ptr_);

	gfx_manager_ptr->add_vertex_attribute(axis_pipeline_id_, 0, /* location */
		VK_FORMAT_R32G32B32A32_SFLOAT,
		0,                 /* offset_in_bytes */
		sizeof(float) * 1, /* stride_in_bytes */
		VK_VERTEX_INPUT_RATE_INSTANCE);
	gfx_manager_ptr->set_pipeline_dsg(axis_pipeline_id_, axis_dsg_ptr_);

	if (N_VERTICES == 144) {
		gfx_manager_ptr->set_input_assembly_properties(
			pipeline_id_, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST);
	} else {
		gfx_manager_ptr->set_input_assembly_properties(
			pipeline_id_, VK_PRIMITIVE_TOPOLOGY_LINE_LIST);
	}

	gfx_manager_ptr->set_rasterization_properties(
		pipeline_id_, VK_POLYGON_MODE_FILL, VK_CULL_MODE_NONE,
		VK_FRONT_FACE_COUNTER_CLOCKWISE, 10.0f /* line_width */);
	gfx_manager_ptr->toggle_depth_test(pipeline_id_, true, /* should_enable */
		VK_COMPARE_OP_LESS_OR_EQUAL);
	gfx_manager_ptr->toggle_depth_writes(pipeline_id_, true); /* should_enable */
	gfx_manager_ptr->toggle_dynamic_states(
		pipeline_id_, true, /* should_enable */
		Anvil::GraphicsPipelineManager::DYNAMIC_STATE_LINE_WIDTH_BIT);

	gfx_manager_ptr->set_input_assembly_properties(
		axis_pipeline_id_, VK_PRIMITIVE_TOPOLOGY_LINE_LIST);
	gfx_manager_ptr->set_rasterization_properties(
		axis_pipeline_id_, VK_POLYGON_MODE_FILL, VK_CULL_MODE_NONE,
		VK_FRONT_FACE_COUNTER_CLOCKWISE, 10.0f /* line_width */);
	gfx_manager_ptr->toggle_depth_test(axis_pipeline_id_, false,
		VK_COMPARE_OP_LESS_OR_EQUAL);
	gfx_manager_ptr->toggle_depth_writes(axis_pipeline_id_, false);
	gfx_manager_ptr->toggle_dynamic_states(
		axis_pipeline_id_, true, /* should_enable */
		Anvil::GraphicsPipelineManager::DYNAMIC_STATE_LINE_WIDTH_BIT);
}

/*
   IMAGE INITIALIZATION.
   Drawn from the AMD DynamicBuffer example.
 */
void App::init_images() {
	for (uint32_t n_depth_image = 0; n_depth_image < N_SWAPCHAIN_IMAGES;
	++n_depth_image) {
		depth_images_[n_depth_image] = Anvil::Image::create_nonsparse(
			device_ptr_, VK_IMAGE_TYPE_2D, VK_FORMAT_D16_UNORM,
			VK_IMAGE_TILING_OPTIMAL, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,
			windowWidth_, windowHeight_, 1, /* in_base_mipmap_depth */
			1,                              /* in_n_layers          */
			VK_SAMPLE_COUNT_1_BIT, Anvil::QUEUE_FAMILY_GRAPHICS_BIT,
			VK_SHARING_MODE_EXCLUSIVE, false, /* in_use_full_mipmap_chain */
			0,                                /* in_memory_features       */
			0,                                /* in_create_flags          */
			VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
			nullptr); /* in_mipmaps_ptr */

		depth_image_views_[n_depth_image] = Anvil::ImageView::create_2D(
			device_ptr_, depth_images_[n_depth_image], 0, /* n_base_layer        */
			0,                                             /* n_base_mipmap_level */
			1,                                             /* n_mipmaps           */
			VK_IMAGE_ASPECT_DEPTH_BIT,
			depth_images_[n_depth_image]->get_image_format(),
			VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY,
			VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY);

		depth_images_[n_depth_image]->set_name_formatted("Depth image [%d]",
			n_depth_image);
		depth_image_views_[n_depth_image]->set_name_formatted(
			"Depth image view [%d]", n_depth_image);
	}
}

// Actually intialize the command buffers.
void App::init_command_buffers() {
	// Boilerplate to prepare the graphics pipeline.
	std::shared_ptr<Anvil::SGPUDevice> device_locked_ptr(device_ptr_);
	std::shared_ptr<Anvil::GraphicsPipelineManager> gfx_pipeline_manager_ptr(
		device_locked_ptr->get_graphics_pipeline_manager());
	const bool is_debug_marker_ext_present(
		device_locked_ptr->is_ext_debug_marker_extension_enabled());
	std::shared_ptr<Anvil::PipelineLayout> computePipelineLayoutPointer;
	VkImageSubresourceRange subresource_range;
	std::shared_ptr<Anvil::Queue> universal_queue_ptr(
		device_locked_ptr->get_universal_queue(0));

	computePipelineLayoutPointer =
		device_locked_ptr->get_compute_pipeline_manager()
		->get_compute_pipeline_layout(compute_pipeline_id_);

	subresource_range.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	subresource_range.baseArrayLayer = 0;
	subresource_range.baseMipLevel = 0;
	subresource_range.layerCount = 1;
	subresource_range.levelCount = 1;

	// Set up rendering command buffers. We need one per swap-chain image.
	for (unsigned int n_current_swapchain_image = 0;
	n_current_swapchain_image < N_SWAPCHAIN_IMAGES;
		++n_current_swapchain_image) {
		std::shared_ptr<Anvil::PrimaryCommandBuffer> draw_cmd_buffer_ptr;
		draw_cmd_buffer_ptr =
			device_locked_ptr->get_command_pool(Anvil::QUEUE_FAMILY_TYPE_UNIVERSAL)
			->alloc_primary_level_command_buffer();

		// Start recording commands.
		draw_cmd_buffer_ptr->start_recording(false,  // One-time submit.
			true);  // Simultaneous use allowed.

// Switch the swap-chain image layout to renderable.
		{
			Anvil::ImageBarrier image_barrier(
				0,                                     // Source access mask.
				VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,  // Destination access mask.
				false, VK_IMAGE_LAYOUT_UNDEFINED,
				VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
				universal_queue_ptr->get_queue_family_index(),
				universal_queue_ptr->get_queue_family_index(),
				swapchain_ptr_->get_image(n_current_swapchain_image),
				subresource_range);

			draw_cmd_buffer_ptr->record_pipeline_barrier(
				VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
				VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
				VK_FALSE,  // in_by_region
				0,         // in_memory_barrier_count
				nullptr,   // in_memory_barrier_ptrs
				0,         // in_buffer_memory_barrier_count
				nullptr,   // in_buffer_memory_barrier_ptrs
				1,         // in_image_memory_barrier_count
				&image_barrier);
		}

		// Invalidate the shader read cache for this CPU-written data.
		Anvil::BufferBarrier view_proj_value_buffer_barrier = Anvil::BufferBarrier(
			VK_ACCESS_HOST_WRITE_BIT, VK_ACCESS_UNIFORM_READ_BIT,
			VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED,
			viewProjUniformPointer,
			n_current_swapchain_image * mat5UniformSizePerSwapchain,
			mat5UniformSizePerSwapchain);
		Anvil::BufferBarrier view_value_buffer_barrier = Anvil::BufferBarrier(
			VK_ACCESS_HOST_WRITE_BIT, VK_ACCESS_UNIFORM_READ_BIT,
			VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED,
			viewMatrixUniformPointer,
			n_current_swapchain_image * mat5UniformSizePerSwapchain,
			mat5UniformSizePerSwapchain);

		draw_cmd_buffer_ptr->record_pipeline_barrier(
			VK_PIPELINE_STAGE_HOST_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
			VK_FALSE,
			0,                        // in_memory_barrier_count
			nullptr,                  // in_memory_barriers_ptr
			1,                        // in_buffer_memory_barrier_count
			&view_proj_value_buffer_barrier,  // in_buffer_memory_barriers_ptr
			0,                        // in_image_memory_barrier_count
			nullptr);                 // in_image_memory_barriers_ptr

		draw_cmd_buffer_ptr->record_pipeline_barrier(
			VK_PIPELINE_STAGE_HOST_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
			VK_FALSE,
			0,                        // in_memory_barrier_count
			nullptr,                  // in_memory_barriers_ptr
			1,                        // in_buffer_memory_barrier_count
			&view_value_buffer_barrier,  // in_buffer_memory_barriers_ptr
			0,                        // in_image_memory_barrier_count
			nullptr);                 // in_image_memory_barriers_ptr

		// Let's generate some sine offset data using our compute shader.
		draw_cmd_buffer_ptr->record_bind_pipeline(VK_PIPELINE_BIND_POINT_COMPUTE,
			compute_pipeline_id_);

		if (is_debug_marker_ext_present) {
			static const float region_color[4] = { 0.0f, 1.0f, 0.0f, 1.0f };
			draw_cmd_buffer_ptr->record_debug_marker_begin_EXT(
				"Sine offset data computation", region_color);
		}

		printf("c0\n");
		std::shared_ptr<Anvil::DescriptorSet> producer_dses[] = {
			compute_dsg_ptr_->get_descriptor_set(0),
			compute_dsg_ptr_->get_descriptor_set(1) };

		printf("c0.1\n");
		static const uint32_t n_producer_dses =
			sizeof(producer_dses) / sizeof(producer_dses[0]);

		printf("c1 n_producer_dses: %d\n", n_producer_dses);
		draw_cmd_buffer_ptr->record_bind_descriptor_sets(
			VK_PIPELINE_BIND_POINT_COMPUTE, computePipelineLayoutPointer,
			0, /* firstSet */
			n_producer_dses, producer_dses, 0, nullptr);

		draw_cmd_buffer_ptr->record_dispatch(1 + (N_MESHES / 512),  /* x */
			1,  /* y */
			1); /* z */

		if (is_debug_marker_ext_present) {
			draw_cmd_buffer_ptr->record_debug_marker_end_EXT();
		}
		printf("c2\n");

		// Now, use the generated data to draw stuff!
		VkClearValue clear_values[2];
		VkRect2D render_area;

		clear_values[0].color.float32[0] = 0.25f;
		clear_values[0].color.float32[1] = 0.50f;
		clear_values[0].color.float32[2] = 0.75f;
		clear_values[0].color.float32[3] = 1.0f;
		clear_values[1].depthStencil.depth = 1.0f;

		render_area.extent.height = windowHeight_;
		render_area.extent.width = windowWidth_;
		render_area.offset.x = 0;
		render_area.offset.y = 0;

		// NOTE: The render-pass switches the swap-chain image back to the
		// presentable layout
		//      after the draw call finishes.
		draw_cmd_buffer_ptr->record_begin_render_pass(
			2,  // n_clear_values
			clear_values, fbos_[n_current_swapchain_image], render_area,
			renderpass_ptr_, VK_SUBPASS_CONTENTS_INLINE);
		{
			std::shared_ptr<Anvil::DescriptorSet> renderer_dses[] = {
				dsg_ptr_->get_descriptor_set(0) };
			const uint32_t n_renderer_dses =
				sizeof(renderer_dses) / sizeof(renderer_dses[0]);

			std::shared_ptr<Anvil::PipelineLayout> renderer_pipeline_layout_ptr;

			renderer_pipeline_layout_ptr =
				gfx_pipeline_manager_ptr->get_graphics_pipeline_layout(
					pipeline_id_);

			draw_cmd_buffer_ptr->record_bind_pipeline(VK_PIPELINE_BIND_POINT_GRAPHICS,
				pipeline_id_);

			static const VkDeviceSize offsets = 0;
			draw_cmd_buffer_ptr->record_bind_vertex_buffers(0,  // startBinding
				1,  // bindingCount
				&inputCubeBufferPointer_,
				&offsets);

			// Set line width.
			float lineWidth = 2;
			draw_cmd_buffer_ptr->record_set_line_width(lineWidth);
			printf("c4\n");

			draw_cmd_buffer_ptr->record_bind_descriptor_sets(
				VK_PIPELINE_BIND_POINT_GRAPHICS, renderer_pipeline_layout_ptr,
				0, /* firstSet */
				n_renderer_dses, renderer_dses, 0, nullptr);

			draw_cmd_buffer_ptr->record_draw((N_MESHES * N_VERTICES), 1, /* instanceCount */
				0,     /* firstVertex   */
				0);    /* firstInstance */

		}
		draw_cmd_buffer_ptr->record_end_render_pass();

		printf("c5\n");
		draw_cmd_buffer_ptr->record_begin_render_pass(
			0,
			nullptr, fbos_[n_current_swapchain_image], render_area,
			axis_renderpass_ptr_, VK_SUBPASS_CONTENTS_INLINE);
		{
			std::shared_ptr<Anvil::DescriptorSet> axis_renderer_dses[] = {
				axis_dsg_ptr_->get_descriptor_set(0) };
			const uint32_t n_axis_renderer_dses =
				sizeof(axis_renderer_dses) / sizeof(axis_renderer_dses[0]);

			std::shared_ptr<Anvil::PipelineLayout> renderer_pipeline_layout_ptr;

			renderer_pipeline_layout_ptr =
				gfx_pipeline_manager_ptr->get_graphics_pipeline_layout(
					axis_pipeline_id_);

			draw_cmd_buffer_ptr->record_bind_pipeline(VK_PIPELINE_BIND_POINT_GRAPHICS,
				axis_pipeline_id_);
			static const VkDeviceSize offsets = 0;
			draw_cmd_buffer_ptr->record_bind_vertex_buffers(0,  // startBinding
				1,  // bindingCount
				&inputCubeBufferPointer_,
				&offsets);
			float lineWidth = 2;
			draw_cmd_buffer_ptr->record_set_line_width(lineWidth);

			draw_cmd_buffer_ptr->record_bind_descriptor_sets(
				VK_PIPELINE_BIND_POINT_GRAPHICS, renderer_pipeline_layout_ptr,
				0, n_axis_renderer_dses, axis_renderer_dses, 0, nullptr);
#ifdef _WIN32
#else
			draw_cmd_buffer_ptr->record_draw(8, 1, 0, 0);
#endif
		}
		draw_cmd_buffer_ptr->record_end_render_pass();

		// Close the recording process.
		draw_cmd_buffer_ptr->stop_recording();
		command_buffers_[n_current_swapchain_image] = draw_cmd_buffer_ptr;
		printf("c6\n");
	}
}

void App::init_camera() {
	camera_ = Camera();
	camera_.UpdateView();
	camera_.UpdateProj();
	camera_.GetViewProj().Print();
	camera_.SetTerrain(MESH_CENTERS);
	Callback::GetInstance()->init(this, &camera_, GetGLFWWindow());
	glfwSetKeyCallback(GetGLFWWindow(), Callback::on_keypress_event);
	glfwSetMouseButtonCallback(GetGLFWWindow(), Callback::on_mouse_button_event);
	glfwSetCursorPosCallback(GetGLFWWindow(), Callback::on_mouse_move_event);
	glfwSetScrollCallback(GetGLFWWindow(), Callback::on_mouse_scroll_event);
	glfwSetInputMode(GetGLFWWindow(), GLFW_CURSOR, GLFW_CURSOR_DISABLED);
}

void App::handle_keys() {
	auto keys = Callback::GetInstance()->get_keys();
	for (int key : *keys) {
		switch (key) {
		case 'w': case 'W':
			camera_.MoveForward(0.1f);
			break;
		case 's': case 'S':
			camera_.MoveBackward(0.1f);
			break;
		case 'a': case 'A':
			camera_.MoveLeft(0.1f);
			break;
		case 'd': case 'D':
			camera_.MoveRight(0.1f);
			break;
		case 'q': case 'Q':
			camera_.MoveAna(0.1f);
			break;
		case 'e': case 'E':
			camera_.MoveKata(0.1f);
			break;
		case 'r': case 'R':
			camera_.MoveUp(0.1f);
			break;
		case 'f': case 'F':
			camera_.MoveDown(0.1f);
			break;
		case '1':
			camera_.RollLeft(0.015f);
			break;
		case '3':
			camera_.RollRight(0.015f);
			break;
		}
	}

	// KEEP for debugging
	// std::cout << "\n";
	// mat5 view = camera_.GetViewProj();
	// (view * vec5(1, 1, 1, 1, 1)).Print();
	//glm::mat4 view4 =
	//   glm::lookAt(glm::vec3(-5, 0, 0), glm::vec3(0, 0, 0), glm::vec3(0, 1, 0));
	// glm::vec4 f = view4 * glm::vec4(1, 2, 3, 4);
	// mat5 view5 = camera_.getView();
	// (view5 * vec5(1, 1, 1, 1, 1)).Print();
	// view5.Print();
	// glm::mat4 proj4 = glm::perspective(30.0, 0.75, 1.0, 20.0);
	// mat5 proj5 = camera_.getProj();
	// glm::mat4 tran = glm::translate(glm::mat4(1), glm::vec3(1, 2, 3));
	// glm::mat4 t2 = view4 * tran;
	// glm::mat4 t3 = glm::translate(view4, glm::vec3(1, 2, 3));
	// std::cout << "view:\n";
	// view5.Print();
	// std::cout << "proj:\n";
	// proj5.Print();
	// std::cout << "viewproj:\n";
	// camera_.GetViewProj().Print();
	// std::cout << "\n";
}

// Toggles between drawing a solid envelope or a wireframe shape.
void App::ToggleRenderMode() {
	printf("Toggling render mode.\n");
	if (N_VERTICES == 144) {
		N_VERTICES = 64;
	} else {
		N_VERTICES = 144;
	}

	vkDeviceWaitIdle(device_ptr_.lock()->get_device_vk());

	frame_signal_semaphores_.clear();
	frame_wait_semaphores_.clear();

	for (uint32_t n_cmd_buffer = 0;
	n_cmd_buffer < sizeof(command_buffers_) / sizeof(command_buffers_[0]);
		++n_cmd_buffer) {
		command_buffers_[n_cmd_buffer] = nullptr;
	}

	for (uint32_t n_depth_image = 0;
	n_depth_image < sizeof(depth_images_) / sizeof(depth_images_[0]);
		++n_depth_image) {
		depth_images_[n_depth_image] = nullptr;
	}

	for (uint32_t n_depth_image_view = 0;
	n_depth_image_view < sizeof(depth_image_views_) / sizeof(depth_image_views_[0]);
		++n_depth_image_view) {
		depth_image_views_[n_depth_image_view] = nullptr;
	}

	for (uint32_t n_fbo = 0;
	n_fbo < sizeof(fbos_) / sizeof(fbos_[0]);
		++n_fbo) {
		fbos_[n_fbo] = nullptr;
	}

	dsg_ptr_.reset();
	fs_ptr_.reset();
	renderpass_ptr_.reset();
	vs_ptr_.reset();
	cs_ptr_.reset();
	compute_dsg_ptr_.reset();
	data_buffer_ptr_.reset();
	mesh_data_buffer_ptr_.reset();
	comp_data_buffer_ptr_.reset();

	//present_queue_ptr_.reset();
	//rendering_surface_ptr_.reset();
	//swapchain_ptr_.reset();
	//window_ptr_.reset();

	//device_ptr_.lock()->destroy();
	//device_ptr_.reset();

	//instance_ptr_->destroy();
	//instance_ptr_.reset();

	// Reinitialize rendering with new settings.
	// init_vulkan();
	// init_window();
	// init_swapchain();
	init_buffers();
	init_dsgs();
	init_images();
	init_semaphores();
	init_shaders();
	init_compute_pipelines();
	init_framebuffers();
	init_gfx_pipelines();
	init_command_buffers();
	// init_camera();
}

/*
  The main development portion of the code, now that boilerplate
  and pipeline setup is completed.
 */

 /*
  Handle the task of drawing a frame for the app.
 */
void App::draw_frame(void* app_raw_ptr) {
	App* app_ptr = static_cast<App*>(app_raw_ptr);
	std::shared_ptr<Anvil::Semaphore> curr_frame_signal_semaphore_ptr;
	std::shared_ptr<Anvil::Semaphore> curr_frame_wait_semaphore_ptr;
	std::shared_ptr<Anvil::SGPUDevice> device_locked_ptr =
		app_ptr->device_ptr_.lock();
	static uint32_t n_frames_rendered = 0;
	uint32_t n_swapchain_image;
	std::shared_ptr<Anvil::Semaphore> present_wait_semaphore_ptr;
	const VkPipelineStageFlags wait_stage_mask =
		VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;

	// Determine the signal + wait semaphores to use for drawing this frame.
	app_ptr->n_last_semaphore_used_ =
		(app_ptr->n_last_semaphore_used_ + 1) % app_ptr->n_swapchain_images_;

	curr_frame_signal_semaphore_ptr =
		app_ptr->frame_signal_semaphores_[app_ptr->n_last_semaphore_used_];
	curr_frame_wait_semaphore_ptr =
		app_ptr->frame_wait_semaphores_[app_ptr->n_last_semaphore_used_];

	present_wait_semaphore_ptr = curr_frame_signal_semaphore_ptr;

	// Determine the semaphore which the swapchain image.
	n_swapchain_image = app_ptr->swapchain_ptr_->acquire_image(
		curr_frame_wait_semaphore_ptr, true);

	// Update View Proj matrix,
	mat5 viewProj = app_ptr->camera_.GetViewProj();
	app_ptr->viewProjUniformPointer->write(
		app_ptr->mat5UniformSizePerSwapchain * n_swapchain_image,  // Offset.
		sizeof(glm::mat4), &viewProj.get_main_mat());
	app_ptr->viewProjUniformPointer->write(
		app_ptr->mat5UniformSizePerSwapchain * n_swapchain_image +
		sizeof(glm::mat4),  // Offset.
		sizeof(glm::vec4),
		&viewProj.get_column());
	app_ptr->viewProjUniformPointer->write(
		app_ptr->mat5UniformSizePerSwapchain * n_swapchain_image +
		sizeof(glm::mat4) + sizeof(glm::vec4),  // Offset.
		sizeof(glm::vec4),
		&viewProj.get_row());
	app_ptr->viewProjUniformPointer->write(
		app_ptr->mat5UniformSizePerSwapchain * n_swapchain_image +
		sizeof(glm::mat4) + 2 * sizeof(glm::vec4),  // Offset.
		sizeof(float),
		&viewProj.get_ww());

	// Update View matrix,
	mat5 view = app_ptr->camera_.getView();
	app_ptr->viewMatrixUniformPointer->write(
		app_ptr->mat5UniformSizePerSwapchain * n_swapchain_image,  // Offset.
		sizeof(glm::mat4), &view.get_main_mat());
	app_ptr->viewMatrixUniformPointer->write(
		app_ptr->mat5UniformSizePerSwapchain * n_swapchain_image +
		sizeof(glm::mat4),  // Offset.
		sizeof(glm::vec4),
		&view.get_column());
	app_ptr->viewMatrixUniformPointer->write(
		app_ptr->mat5UniformSizePerSwapchain * n_swapchain_image +
		sizeof(glm::mat4) + sizeof(glm::vec4),  // Offset.
		sizeof(glm::vec4),
		&view.get_row());
	app_ptr->viewMatrixUniformPointer->write(
		app_ptr->mat5UniformSizePerSwapchain * n_swapchain_image +
		sizeof(glm::mat4) + 2 * sizeof(glm::vec4),  // Offset.
		sizeof(float),
		&view.get_ww());

	/* Submit jobs to relevant queues and make sure they are correctly
	 * synchronized */
	device_locked_ptr->get_universal_queue(0)
		->submit_command_buffer_with_signal_wait_semaphores(
			app_ptr->command_buffers_[n_swapchain_image],
			1,                                   /* n_semaphores_to_signal */
			&curr_frame_signal_semaphore_ptr, 1, /* n_semaphores_to_wait_on */
			&curr_frame_wait_semaphore_ptr, &wait_stage_mask,
			false, /* should_block */
			nullptr);

	app_ptr->present_queue_ptr_->present(
		app_ptr->swapchain_ptr_, n_swapchain_image, 1, /* n_wait_semaphores */
		&present_wait_semaphore_ptr);

	++n_frames_rendered;

#if defined(ENABLE_OFFSCREEN_RENDERING)
	{
		if (n_frames_rendered >= N_FRAMES_TO_RENDER) {
			window_ptr_->close();
		}
	}
#endif

	// Read all data points back.
	if (DEBUG_REREAD) {
		for (int i = 0; i < N_MESHES*N_VERTICES; ++i) {
			if (i != 32 && i != 33) continue;
			glm::vec4 input, output;
			/*
			app_ptr->inputCubeBufferPointer_->read(
				app_ptr->inputCubeElementOffsets_[i] + 0 * sizeof(float),
				sizeof(float), &input.x);
			app_ptr->inputCubeBufferPointer_->read(
				app_ptr->inputCubeElementOffsets_[i] + 1 * sizeof(float),
				sizeof(float), &input.y);
			app_ptr->inputCubeBufferPointer_->read(
				app_ptr->inputCubeElementOffsets_[i] + 2 * sizeof(float),
				sizeof(float), &input.z);
			app_ptr->inputCubeBufferPointer_->read(
				app_ptr->inputCubeElementOffsets_[i] + 3 * sizeof(float),
				sizeof(float), &input.w);*/
			app_ptr->outputCubeVerticesBufferPointer_->read(
				app_ptr->outputCubeVerticesBufferSizes_[i] + 0 * sizeof(float),
				sizeof(float), &output.x);
			app_ptr->outputCubeVerticesBufferPointer_->read(
				app_ptr->outputCubeVerticesBufferSizes_[i] + 1 * sizeof(float),
				sizeof(float), &output.y);
			app_ptr->outputCubeVerticesBufferPointer_->read(
				app_ptr->outputCubeVerticesBufferSizes_[i] + 2 * sizeof(float),
				sizeof(float), &output.z);
			app_ptr->outputCubeVerticesBufferPointer_->read(
				app_ptr->outputCubeVerticesBufferSizes_[i] + 3 * sizeof(float),
				sizeof(float), &output.w);
			if (output.x < 1 && output.x > -1 &&
				output.y < 1 && output.y > -1 &&
				output.z < 1 && output.z > -1) {
				std::cout << "FOUND ONE";
			}
			if (output.z > 100) {
				std::cout << "FAR";
			}

			//std::cout << "i offset: " << app_ptr->inputCubeElementOffsets_[i] << "\n";
			std::cout << "o offset: " << i << " "
				<< app_ptr->outputCubeVerticesBufferSizes_[i] << "\n";
			//std::cout << "i (" << input.x << ", " << input.y << ", " << input.z
			//          << ", " << input.w << ")\n";
			std::cout << "o (" << output.x << ", " << output.y << ", " << output.z
				<< ", " << output.w << ")\n\n";
		}
	}
}

void App::run() { //window_ptr_->run(); 
	while (!ShouldQuit()) {
		glfwPollEvents();
		draw_frame(this);
		if (DEBUG_FRAME_TIME && !DEBUG_BAKE_TIME) {
			auto cur_time = std::chrono::steady_clock::now();
			std::chrono::duration<double, std::milli> dif = cur_time - prev_time;
			std::cout << dif.count() << "\n";
			prev_time = cur_time;
		}
		handle_keys();
	}
	DestroyWindow();
}

VkBool32 App::on_validation_callback(VkDebugReportFlagsEXT message_flags,
	VkDebugReportObjectTypeEXT object_type,
	const char* layer_prefi,
	const char* message, void* user_arg) {
	// Display any detected error.
	if ((message_flags & VK_DEBUG_REPORT_ERROR_BIT_EXT) != 0) {
		fprintf(stderr, "[!] %s\n", message);
	}
	return false;
}
