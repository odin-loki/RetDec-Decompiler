/**
* @file include/retdec/ctypes/annotation_out.h
* @brief A representation of @c out annotation.
* @copyright (c) 2017 Odin Loch Trading as Imortek
*/

#ifndef RETDEC_CTYPES_ANNOTATION_OUT_H
#define RETDEC_CTYPES_ANNOTATION_OUT_H

#include <memory>
#include "retdec/ctypes/annotation.h"

namespace retdec {
namespace ctypes {

/**
* @brief A representation of @c out annotation.
*/
class AnnotationOut: public Annotation
{
	public:
		AnnotationOut(const std::string &name): Annotation(name) {};
		static std::shared_ptr<AnnotationOut> create(
			const std::shared_ptr<Context> &context,
			const std::string &name);

		virtual bool isOut() const override;
};

} // namespace ctypes
} // namespace retdec

#endif
