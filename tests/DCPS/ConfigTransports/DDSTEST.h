/*
 * $Id$
 *
 *
 * Distributed under the OpenDDS License.
 * See: http://www.opendds.org/license.html
 */

#ifndef OPENDDS_DCPS_DDS_TEST_IMPL_H
#define OPENDDS_DCPS_DDS_TEST_IMPL_H

#include <string>
#include "dds/DCPS/dcps_export.h"

namespace OpenDDS {
namespace DCPS {
 class TransportClient;
 class EntityImpl;

};
};

namespace DDS {
class DataReader;
class DataWriter;
class Entity;
class DomainParticipant;
};

//class OpenDDS::DCPS::TransportClient;
//class DDS::DataReader;
//class DDS::DataWriter;
//class DDS::Entity;
//class DDS::DomainParticipant;

//class TransportClient;
//class DataReader;
//class DataWriter;
//class Entity;
//class DomainParticipant;

//namespace {
/**
 * @brief A bridge for tests that need access to non-public parts of the transport framework
 *
 */
class OpenDDS_Dcps_Export DDS_TEST {
public:
    static int supports(const DDS::DataReader* pub, const std::string& name);
    static int supports(const DDS::Entity* pub, const std::string& name);
    static int supports(const DDS::DataWriter* pub, const std::string& name);
    static int supports(const DDS::DomainParticipant* tc, const std::string& name);

    static int supports(const OpenDDS::DCPS::TransportClient* tc, const std::string& name);
    static int supports(const OpenDDS::DCPS::EntityImpl* entity, const std::string& protocol_name);

protected:

    DDS_TEST() {
    };

    virtual ~DDS_TEST() {
    };

};

//};

#endif
