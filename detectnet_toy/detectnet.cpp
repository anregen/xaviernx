/*
 * Copyright (c) 2020, NVIDIA CORPORATION. All rights reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */

#include <jetson-utils/videoSource.h>
#include <jetson-utils/videoOutput.h>
#include <jetson-utils/cudaCrop.h>
#include <jetson-utils/cudaMappedMemory.h>

#include <jetson-inference/detectNet.h>

#include <signal.h>
#include <string.h>


#ifdef HEADLESS
	#define IS_HEADLESS() "headless"	// run without display
#else
	#define IS_HEADLESS() (const char*)NULL
#endif


bool signal_recieved = false;

void sig_handler(int signo)
{
	if( signo == SIGINT )
	{
		LogVerbose("received SIGINT\n");
		signal_recieved = true;
	}
}

int usage()
{
	printf("usage: detectnet_toy [--help] [--network=NETWORK] [--threshold=THRESHOLD] ...\n");
	printf("                 input_URI [output_URI]\n\n");
	printf("Locate objects in a video/image stream using an object detection DNN.\n");
	printf("See below for additional arguments that may not be shown above.\n\n");
	printf("positional arguments:\n");
	printf("    input_URI       resource URI of input stream  (see videoSource below)\n");
	printf("    output_URI      resource URI of output stream (see videoOutput below)\n\n");
	printf("    image_URI       resource URI of image output  (see videoOutput below)\n\n");
  printf("    overlay_flags   defaults to \"box,labels,conf\"\n\n");

	printf("%s", detectNet::Usage());
	printf("%s", videoSource::Usage());
	printf("%s", videoOutput::Usage());
	printf("%s", Log::Usage());

	return 0;
}

int main( int argc, char** argv )
{
	/*
	 * parse command line
	 */
	commandLine cmdLine(argc, argv, IS_HEADLESS());

	if( cmdLine.GetFlag("help") )
		return usage();


	/*
	 * attach signal handler
	 */
	if( signal(SIGINT, sig_handler) == SIG_ERR )
		LogError("can't catch SIGINT\n");


	/*
	 * create input stream
	 */
	videoSource* input = videoSource::Create(cmdLine, ARG_POSITION(0));

	if( !input )
	{
		LogError("detectnet:  failed to create input stream\n");
		return 0;
	}


	/*
	 * create output stream
	 */
	videoOutput* output = videoOutput::Create(cmdLine, ARG_POSITION(1));
	
	if( !output )
		LogError("detectnet:  failed to create output stream\n");	
	
	videoOutput* img_output = videoOutput::Create(cmdLine, ARG_POSITION(2));
	if( !img_output )
		LogError("detectnet:  failed to create image output object\n");	
	
  // learn how to use cmdLine
  const char * overlayFlagString = "none";
  // if (argc > 3)
  // char * overlayFlagString = argv[4];
  //if( !overlayFlagString )
  //  overlayFlagString = "box,labels,conf";


	/*
	 * create detection network
	 */
	detectNet* net = detectNet::Create(cmdLine);
	
	if( !net )
	{
		LogError("detectnet:  failed to load detectNet model\n");
		return 0;
	}

	// parse overlay flags
	const uint32_t overlayFlags = detectNet::OverlayFlagsFromStr(cmdLine.GetString("overlay", overlayFlagString));
	
	uchar3* cropped_image = NULL;
	const uint32_t CROP_WIDTH = 416;
	const uint32_t CROP_HEIGHT = 416;
	unsigned int lr_center, tb_center = 0;
	int4 roi = make_int4(0, 0, CROP_WIDTH, CROP_HEIGHT );
	cudaAllocMapped(&cropped_image, CROP_WIDTH, CROP_HEIGHT);
	const float MIN_CONF = 0.70;
	const int IMAGE_PACING = 20;  // will skip at least 20 frames between image captures
	long int pacer = -1*(IMAGE_PACING + 1);  // so the first video frame can pass the pace check
  long int frame_count = 0;          
	const std::string OBJECT = "dog";
	bool detect_hit = false;

	/*
	 * processing loop
	 */
	while( !signal_recieved )
	{
		// capture next image image
		uchar3* image = NULL;

		if( !input->Capture(&image, 1000) )
		{
			// check for EOS
			if( !input->IsStreaming() )
				break; 

			LogError("detectnet:  failed to capture video frame\n");
			continue;
		}

    frame_count++;

		// detect objects in the frame
		detectNet::Detection* detections = NULL;
	
		const int numDetections = net->Detect(image, input->GetWidth(), input->GetHeight(), &detections, overlayFlags);
		
		if( numDetections > 0 )
		{
			LogVerbose("%i objects detected\n", numDetections);
      detect_hit = false;
		
			for( int n=0; n < numDetections; n++ )
			{
				LogVerbose("detected obj %i  class #%u (%s)  confidence=%f\n", n, detections[n].ClassID, net->GetClassDesc(detections[n].ClassID), detections[n].Confidence);
				LogVerbose("bounding box %i  (%f, %f)  (%f, %f)  w=%f  h=%f\n", n, detections[n].Left, detections[n].Top, detections[n].Right, detections[n].Bottom, detections[n].Width(), detections[n].Height()); 
				if (detections[n].Confidence > MIN_CONF){
					std::string desc = net->GetClassDesc(detections[n].ClassID);
					detect_hit = (desc.find(OBJECT) != std::string::npos);
					detect_hit &= (detections[n].Width() <= CROP_WIDTH) && (detections[n].Height() <= CROP_HEIGHT);
					if (detect_hit){
            LogVerbose("hit detected\n");
            if (frame_count - pacer > IMAGE_PACING)
            {
              pacer = frame_count;
						  lr_center = (detections[n].Left + detections[n].Width()/2);
						  tb_center = (detections[n].Top + detections[n].Height()/2);
						  lr_center = lr_center < CROP_WIDTH/2 ? CROP_WIDTH/2 : lr_center;
						  lr_center = lr_center > input->GetWidth() - CROP_WIDTH/2 ? input->GetWidth() - CROP_WIDTH/2 : lr_center;
						  tb_center = tb_center < CROP_HEIGHT/2 ? CROP_HEIGHT/2 : tb_center;
						  tb_center = tb_center > input->GetHeight() - CROP_HEIGHT/2 ? input->GetHeight() - CROP_HEIGHT/2 : tb_center;

						  roi.x = lr_center - CROP_WIDTH/2;
						  roi.y = tb_center - CROP_HEIGHT/2;
						  roi.z = lr_center + CROP_WIDTH/2;
						  roi.w = tb_center + CROP_HEIGHT/2;
				      
              cudaCrop(image, cropped_image, roi, input->GetWidth(), input->GetHeight());
              img_output->Render(cropped_image, CROP_WIDTH, CROP_HEIGHT);
            }
					}
				}
			}
		}	

		// render outputs
		if( output != NULL )
		{
			output->Render(image, input->GetWidth(), input->GetHeight());

			// update the status bar
			char str[256];
			sprintf(str, "TensorRT %i.%i.%i | %s | Network %.0f FPS", NV_TENSORRT_MAJOR, NV_TENSORRT_MINOR, NV_TENSORRT_PATCH, precisionTypeToStr(net->GetPrecision()), net->GetNetworkFPS());
			output->SetStatus(str);

			// check if the user quit
			if( !output->IsStreaming() )
				signal_recieved = true;
		}

		// print out timing info
		net->PrintProfilerTimes();
	}
	

	/*
	 * destroy resources
	 */
	LogVerbose("detectnet:  shutting down...\n");
	
	SAFE_DELETE(input);
	SAFE_DELETE(output);
	SAFE_DELETE(img_output);
	SAFE_DELETE(net);
	SAFE_DELETE(cropped_image);

	LogVerbose("detectnet:  shutdown complete.\n");
	return 0;
}

