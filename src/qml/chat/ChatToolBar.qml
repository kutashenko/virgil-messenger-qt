import QtQuick 2.12
import QtQuick.Controls 2.12
import QtQuick.Layouts 1.1

ToolBar {

    spacing: 20
    height: toolbarHeight

    background: Rectangle {
        color: toolbarColor
    }

    RowLayout {
        anchors.fill: parent
        Label {
            text: qsTr("Contacts")
            font.pixelSize: 20
            color: toolbarTextColor
            font.bold: true
            Layout.alignment: Qt.AlignLeft
            Layout.leftMargin: 10
        }

        ToolButton {
            id: settingsBtn
            Layout.alignment: Qt.AlignRight
            Layout.rightMargin: 10
            Layout.minimumHeight: 40
            Layout.minimumWidth: 40
            background: Rectangle {
                color: "transparent"
            }

            onClicked: {
                addContact()
            }

            Image {
                source: "qrc:/qml/resources/AddAccount.png"
                width: 30
                height: 30
                anchors.verticalCenter: parent.verticalCenter
                anchors.horizontalCenter: settingsBtn.horizontalCenter
            }
        }
    }
}
