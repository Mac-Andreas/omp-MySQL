/* =========================================
 *
 *  MySQL for open.mp  —  binary entry point
 *
 *  Clean-room implementation. See NOTICE.md.
 *  Author: Xyranaut (Mac Andreas)  ·  License: MIT (see LICENSE)
 *
 * ========================================= */

#include <sdk.hpp>
#include "MySQLComponent.hpp"

// Called by open.mp when the compiled component is loaded.
COMPONENT_ENTRY_POINT()
{
	return MySQLComponent::getInstance();
}
