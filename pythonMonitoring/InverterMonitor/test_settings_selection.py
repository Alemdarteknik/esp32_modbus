import unittest
from unittest.mock import Mock
from settings_selection import select_setting_row


class SelectionTests(unittest.TestCase):
    def test_filtered_row_maps_to_exact_key(self):
        monitor = Mock()
        monitor.snapshot.return_value = {'jobs': [], 'pending': None}
        state = {'table': {'selection': {'rows': [1]}}}
        select_setting_row(state, 'table', ('same name | first', 'same name | second'), monitor)
        self.assertEqual(state['edit_setting'], 'same name | second')
        monitor.write.assert_not_called()

    def test_pending_write_keeps_editor(self):
        monitor = Mock()
        monitor.snapshot.return_value = {'jobs': [], 'pending': {'operation': 'set'}}
        state = {'edit_setting': 'old', 'table': {'selection': {'rows': [0]}}}
        select_setting_row(state, 'table', ('new',), monitor)
        self.assertEqual(state['edit_setting'], 'old')

    def test_empty_or_invalid_selection_keeps_editor(self):
        monitor = Mock()
        monitor.snapshot.return_value = {'jobs': [], 'pending': None}
        for rows in ([], [4], [-1]):
            state = {'edit_setting': 'old', 'table': {'selection': {'rows': rows}}}
            select_setting_row(state, 'table', ('new',), monitor)
            self.assertEqual(state['edit_setting'], 'old')
