#ifndef __FONTINST_INTERFACE_H__
#define __FONTINST_INTERFACE_H__

#include "FontInst.h"
#include "FontinstIface.h"

namespace KFI
{
class FontInstInterface : public OrgKdeFontinstInterface
{
public:
    FontInstInterface()
        : OrgKdeFontinstInterface(OrgKdeFontinstInterface::staticInterfaceName(), FONTINST_PATH, QDBusConnection::sessionBus(), nullptr)
    {
    }
};

}

#endif
