// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once
#include "CoreMinimal.h"
#include <vector>
/*
Like many third party headers, OpenCV headers require some care when importing.

When including opencv headers, the includes should be wrapped like this:

	OPENCV_INCLUDES_START
	#undef check 

	// your opencv include directives go here...

	OPENCV_INCLUDES_END

Note that the #undef directive is required. The START/END macros will ensure that 
the previous value is saved and restored, but due to limits of the preprocessor 
cannot undefine the value.

*/

#if PLATFORM_WINDOWS
#define OPENCV_INCLUDES_START THIRD_PARTY_INCLUDES_START \
	__pragma(warning(disable: 4190))  /* 'identifier1' has C-linkage specified, but returns UDT 'identifier2' which is incompatible with C */ \
	__pragma(warning(disable: 6297))  /* Arithmetic overflow:  32-bit value is shifted, then cast to 64-bit value.  Results might not be an expected value. */ \
	__pragma(warning(disable: 6294))  /* Ill-defined for-loop:  initial condition does not satisfy test.  Loop body not executed. */ \
	__pragma(warning(disable: 6201))  /* Index '<x>' is out of valid index range '<a>' to '<b>' for possibly stack allocated buffer '<variable>'. */ \
	__pragma(warning(disable: 6269))  /* Possibly incorrect order of operations:  dereference ignored. */ \
	__pragma(warning(disable: 4263)) /* cv::detail::BlocksCompensator::feed member function does not override any base class virtual member function */ \
	__pragma(warning(disable: 4264)) /* cv::detail::ExposureCompensator::feed : no override available for virtual member function from base 'cv::detail::ExposureCompensator'; function is hidden */ \
	UE_PUSH_MACRO("check")
#else
// TODO: when adding support for other platforms, this definition may require updating
#define OPENCV_INCLUDES_START THIRD_PARTY_INCLUDES_START UE_PUSH_MACRO("check")
#endif

#define OPENCV_INCLUDES_END THIRD_PARTY_INCLUDES_END UE_POP_MACRO("check")

#if WITH_OPENCV

class UTexture2D;
class FString;
class FName;

namespace cv
{
	class Mat;
	template<typename _Tp> class Point_;
	template<typename _Tp> class Point3_;

	typedef Point_<float> Point2f;
	typedef Point3_<float> Point3f;
};

class OPENCVHELPER_API FOpenCVHelper
{
public:

	/**
	 * Creates a Texture from the given Mat, if its properties (e.g. pixel format) are supported.
	 * 
	 * @param Mat The OpenCV Mat to convert.
	 * @param PackagePath Optional path to a package to create the texture in.
	 * @param TextureName Optional name for the texture. Required if PackagePath is not nullptr.
	 * 
	 * @return Texture created out of the given OpenCV Mat.
	 */
	static UTexture2D* TextureFromCvMat(cv::Mat& Mat, const FString* PackagePath = nullptr, const FName* TextureName = nullptr);
	static UTexture2D* TextureFromCvMat(cv::Mat& Mat, UTexture2D* InTexture);

	static double ComputeReprojectionError(const FTransform& CameraPose, const cv::Mat& CameraIntrinsicMatrix, const std::vector<cv::Point3f>& Points3d, const std::vector<cv::Point2f>& Points2d);
};

#endif //WITH_OPENCV
